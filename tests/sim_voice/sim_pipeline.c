/*
 * Host replica of the fixed_delay pipeline (audio_pipeline_t1.c, then
 * audio_pipeline_t0.c), calling the firmware's own front end, DOA, beamformer,
 * high-pass, VNR hold and echo gate code and the lib_voice
 * AEC, IC and NS. Not modelled: the AGC (channel 0 only, not part of the wake
 * word path), the 48 kHz resamplers and the I2S/SPI transport.
 */
#include <string.h>

#include "aec.h"
#include "aec_priv.h"
#include "ic.h"
#include "ns.h"

#include "audio/doa.h"
#include "audio/echo_gate.h"
#include "audio/mic_frontend.h"
#include "audio/mic_hpf.h"
#include "audio/vnr_hold.h"

#include "sim_pipeline.h"

/* The AEC task schedule build.sh generated */
extern aec_task_distribution_t tdist;

#define PLAYBACK_ENERGY_THRESHOLD 1e-7f
#define IC_VNR_ALPHA 0.97f

static sim_cfg_t cfg;

static aec_state_t aec;

static ic_state_t ic;
static ns_state_t ns;
static doa_state_t doa;
static mic_frontend_state_t fe;
static mic_hpf_t hpf_lo;
static mic_hpf_t hpf_hi;
static vnr_hold_t vnr_hold;
static echo_gate_t gate;
static float vnr_ema;
static uint32_t steer_seen;     /* beam azimuth << 8 | ch0 mic mask of the previous frame */

/*
 * Experiment (not in the firmware, see docs/voice-simulation.md): clear the IC
 * filter whenever the front end changes what feeds the two channels, as
 * the IC's own ic_reset_filter() does on instability.
 */
static void ic_clear_filter(ic_state_t *state)
{
    aec_priv_reset_filter(state->h_hat_bfp[0], IC_X_CHANNELS, IC_FILTER_PHASES);
    bfp_s32_set(&state->sigma_XX_bfp[0], 0, AEC_ZEROVAL_EXP);
}

/* The IC needs the VNR model only for ic_calc_vnr_pred(), which is not used: the VNR is an oracle */
void vnr_feature_state_init(vnr_feature_state_t *s) { (void)s; }
int32_t vnr_inference_init(void) { return 0; }
void vnr_extract_features(vnr_feature_state_t *s, bfp_s32_t *p, int32_t *d, const bfp_complex_s32_t *x)
{
    (void)s; (void)p; (void)d; (void)x;
}
void vnr_inference(float_s32_t *o, bfp_s32_t *p) { (void)p; *o = f32_to_float_s32(0.0f); }

static unsigned aec_mics(void) { return cfg.aec4 ? 4 : 2; }
static unsigned aec_refs(void) { return cfg.aec4 ? 1 : (unsigned)cfg.aec_ref_channels; }

void sim_pipeline_init(const sim_cfg_t *c)
{
    cfg = *c;
    memset(&aec, 0, sizeof(aec));
    aec_init(&aec, aec_mics(), aec_refs(), 10, 5, &tdist);
    ic_init(&ic);
    ns_init(&ns);
    doa_init(&doa);
    if (cfg.aec4 && cfg.doa_post_aec) {
        doa_set_playback_hangover(&doa, DOA_AEC_PLAYBACK_HANGOVER_FRAMES);
    }
    mic_frontend_init(&fe);
    mic_hpf_init(&hpf_lo);
    mic_hpf_init(&hpf_hi);
    vnr_ema = IC_INIT_INPUT_VNR_PRED;
    vnr_hold_init(&vnr_hold, vnr_ema);
    echo_gate_init(&gate);
    steer_seen = UINT32_MAX;
}

static void ref_mono(const int32_t *l, const int32_t *r, int32_t *out)
{
    for (unsigned i = 0; i < SIM_FRAME; i++) {
        switch (cfg.ref_mix) {
        case REF_MIX_LEFT:  out[i] = l[i]; break;
        case REF_MIX_RIGHT: out[i] = r[i]; break;
        default:            out[i] = (l[i] >> 1) + (r[i] >> 1); break;
        }
    }
}

static void frontend(int32_t *mics, int32_t samples[2][SIM_FRAME], const doa_result_t *d, int wake, int conversation)
{
    mic_frontend_set_steering(&fe, cfg.policy, conversation);
    mic_frontend_update(&fe, cfg.mode, d);
    if (wake) {
        (void)mic_frontend_wake_word(&fe, cfg.mode);
    }
    mic_frontend_process(&fe, cfg.mode, mics, &samples[0][0], SIM_FRAME);
}

void sim_pipeline_frame(const int32_t raw_in[4][SIM_FRAME],
                        const int32_t *ref_l, const int32_t *ref_r,
                        float vnr, int wake, int conversation,
                        sim_out_t *out)
{
    static int32_t __attribute__((aligned(8))) mics[4][SIM_FRAME];
    static int32_t __attribute__((aligned(8))) ref[2][SIM_FRAME];
    static int32_t __attribute__((aligned(8))) samples[2][SIM_FRAME];
    memcpy(mics, raw_in, sizeof(mics));

    /* Reference slots as the firmware fills them (main.c) */
    if (aec_refs() == 1) {
        ref_mono(ref_l, ref_r, ref[0]);
        memcpy(ref[1], ref[0], sizeof(ref[0]));
    } else {
        memcpy(ref[0], ref_l, sizeof(ref[0]));
        memcpy(ref[1], ref_r, sizeof(ref[1]));
    }
    float_s32_t max_ref;
    const int playback = audio_block_energies_s32(&ref[0][0], 2, SIM_FRAME, &max_ref) > PLAYBACK_ENERGY_THRESHOLD;

    out->doa_gated = 0;
    if (!cfg.aec4) {
        /* stage_frontend(), stage_aec() */
        out->doa = *doa_process(&doa, &mics[0][0], playback);
        out->doa_gated = playback;
        frontend(&mics[0][0], samples, &out->doa, wake, conversation);
        if (cfg.hpf) {
            mic_hpf_process(&hpf_lo, &samples[0][0], SIM_FRAME);
        }
        out->aec_in_energy = audio_mean_square_s32(&samples[0][0], 2 * SIM_FRAME);
        aec_process_frame(&aec, samples, NULL, samples, ref, NULL);
        out->aec_out_energy = audio_mean_square_s32(&samples[0][0], 2 * SIM_FRAME);
    } else {
        /* stage_capture(), stage_aec(), stage_frontend_after_aec() */
        if (!cfg.doa_post_aec) {
            out->doa = *doa_process(&doa, &mics[0][0], playback);
            out->doa_gated = playback;
        }
        if (cfg.hpf) {
            mic_hpf_process(&hpf_lo, &mics[0][0], SIM_FRAME);
            mic_hpf_process(&hpf_hi, &mics[2][0], SIM_FRAME);
        }
        out->aec_in_energy = audio_mean_square_s32(&mics[0][0], 4 * SIM_FRAME);
        aec_process_frame(&aec, mics, NULL, mics, ref, NULL);
        out->aec_out_energy = audio_mean_square_s32(&mics[0][0], 4 * SIM_FRAME);
        if (cfg.doa_post_aec) {
            const int echo = echo_gate_update(&gate, playback, out->aec_in_energy, out->aec_out_energy);
            out->doa = *doa_process(&doa, &mics[0][0], echo);
            out->doa_gated = echo;
        }
        frontend(&mics[0][0], samples, &out->doa, wake, conversation);
    }
    mic_apply_gain_q16(&samples[0][0], 2 * SIM_FRAME, mic_gain_db_to_q16(cfg.mic_gain_db));

    out->beam_azimuth = mic_frontend_get_beam_azimuth(&fe, cfg.mode);
    uint8_t m0;
    uint8_t m1;
    mic_frontend_get_masks(&fe, cfg.mode, &m0, &m1);
    out->ch0_mic = m0;

    /* Tile 0: stage_vnr_and_ic(), stage_ns() */
    const uint32_t steer = ((uint32_t)out->beam_azimuth << 8) | m0;
    if (cfg.ic_steer_reset && steer_seen != UINT32_MAX && steer != steer_seen) {
        ic_clear_filter(&ic);
    }
    steer_seen = steer;

    if (cfg.ch1_tap == 2) {
        memcpy(out->ch1, samples[0], sizeof(out->ch1));
    }
    ic_filter(&ic, samples[0], samples[1], samples[0]);
    vnr_ema = IC_VNR_ALPHA * vnr_ema + (1.0f - IC_VNR_ALPHA) * vnr;
    const float hold = vnr_hold_update(&vnr_hold, vnr);
    float adapt = vnr_ema;
    if (cfg.ic_fast_hold) {
        adapt = vnr_hold_adapt_vnr(hold, vnr_ema, (float)IC_INIT_INPUT_VNR_THRESHOLD_HIGH);
    }
    /* ic_adapt() reads the VNR from the state, as audio_pipeline_t0.c sets it */
    ic.vnr_pred_state.input_vnr_pred = f32_to_float_s32(adapt);
    ic_adapt(&ic);
    if (cfg.ch1_tap == 1) {
        memcpy(out->ch1, samples[0], sizeof(out->ch1));
    }
    ns_process_frame(&ns, samples[0], samples[0]);
    if (cfg.ch1_tap == 0) {
        memcpy(out->ch1, samples[0], sizeof(out->ch1));
    }
    memcpy(out->ch0, samples[0], sizeof(out->ch0));
}
