/*
 * Host replica of the fixed_delay pipeline (audio_pipeline_t1.c, then
 * audio_pipeline_t0.c), calling the firmware's own front end, DOA, beamformer,
 * high-pass, VNR hold and echo gate code and the lib_voice
 * AEC, IC and NS. Not modelled: the AGC (channel 0 only, not part of the wake
 * word path), the 48 kHz resamplers and the I2S/SPI transport.
 */
#include <math.h>
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
#include "sim_sdbeam.h"

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

static sdb_t sdb;

static const uint16_t *replay_beam;
static const uint8_t *replay_ch0;
static size_t replay_frames;
static size_t frame_no;

void sim_pipeline_set_replay(const uint16_t *beam, const uint8_t *ch0_mic, size_t frames)
{
    replay_beam = beam;
    replay_ch0 = ch0_mic;
    replay_frames = beam ? frames : 0;
}

/* idle_select: per fixed beam, fast and slow speech-band power */
#define SEL_FAST_ALPHA      0.6f    /* ~30 ms */
#define SEL_SLOW_ALPHA      0.998f  /* ~7.5 s */
#define SEL_NOVELTY_DB      6.0f    /* fast over slow power for a beam to be a candidate */
#define SEL_CONFIRM_FRAMES  3       /* consecutive frames a new candidate must lead */
#define SEL_MIN_DWELL       40      /* frames (~0.6 s) before the selection moves again */
static float sel_fast[SDB_MULTI];
static float sel_slow[SDB_MULTI];
static unsigned sel_beam;
static unsigned sel_candidate;
static unsigned sel_count;
static unsigned sel_dwell;

/* mic_mismatch: per mic gain and first order high-pass, applied to the raw mics */
static const float mm_gain_db[4] = { 0.8f, -0.6f, 0.3f, -0.9f };
static const float mm_corner_hz[4] = { 50.0f, 80.0f, 60.0f, 100.0f };
static float mm_x1[4];
static float mm_y1[4];

static void apply_mic_mismatch(int32_t mics[4][SIM_FRAME])
{
    for (unsigned m = 0; m < 4; m++) {
        const float g = powf(10.0f, mm_gain_db[m] / 20.0f);
        const float a = 1.0f / (1.0f + 2.0f * 3.14159265f * mm_corner_hz[m] / 16000.0f);
        for (unsigned i = 0; i < SIM_FRAME; i++) {
            const float x = (float)mics[m][i] / 2147483648.0f;
            const float y = a * (mm_y1[m] + x - mm_x1[m]);
            mm_x1[m] = x;
            mm_y1[m] = y;
            mics[m][i] = (int32_t)lrintf(fmaxf(-1.0f, fminf(0.9999999f, g * y)) * 2147483648.0f);
        }
    }
}

/*
 * doa_vnr_gate: a frame the VNR does not rate as speech goes through the DOA
 * as a non-speech frame. Forcing the noise floor up makes doa_process() see
 * the frame as quiet (its floor tracking drops to the frame energy); the floor
 * is then restored to what the frame would have made it.
 */
static const doa_result_t *doa_step(const int32_t *mics, int gate, float vnr)
{
    if (!cfg.doa_vnr_gate || vnr >= 0.5f) {
        return doa_process(&doa, mics, gate);
    }
    const float floor = doa.noise_floor;
    doa.noise_floor = 1e30f;
    const doa_result_t *r = doa_process(&doa, mics, gate);
    const float energy = doa.noise_floor;
    doa.noise_floor = (floor <= 0.0f || energy < floor) ? energy : floor * DOA_NOISE_FLOOR_RISE;
    return r;
}

static void idle_select_update(float vnr)
{
    float p[SDB_MULTI];
    sdb_multi_power(&sdb, p);
    unsigned best = 0;
    float best_nov = -1e9f;
    for (unsigned b = 0; b < SDB_MULTI; b++) {
        sel_fast[b] = SEL_FAST_ALPHA * sel_fast[b] + (1.0f - SEL_FAST_ALPHA) * p[b];
        sel_slow[b] = sel_slow[b] <= 0.0f ? p[b] : SEL_SLOW_ALPHA * sel_slow[b] + (1.0f - SEL_SLOW_ALPHA) * p[b];
        const float nov = 10.0f * log10f((sel_fast[b] + 1e-20f) / (sel_slow[b] + 1e-20f));
        if (nov > best_nov) {
            best_nov = nov;
            best = b;
        }
    }
    if (sel_dwell < SEL_MIN_DWELL) {
        sel_dwell++;
    }
    const int speech = !cfg.doa_vnr_gate || vnr >= 0.5f;
    if (speech && best_nov > SEL_NOVELTY_DB && best != sel_beam) {
        sel_count = (best == sel_candidate) ? sel_count + 1 : 1;
        sel_candidate = best;
        if (sel_count >= SEL_CONFIRM_FRAMES && sel_dwell >= SEL_MIN_DWELL) {
            sel_beam = best;
            sel_dwell = 0;
            sel_count = 0;
        }
    } else {
        sel_count = 0;
    }
}

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
    sdb_init(&sdb, cfg.sd_wng_db, MIC_FRONTEND_BEAM_INITIAL_DEG);
    frame_no = 0;
    memset(sel_fast, 0, sizeof(sel_fast));
    memset(sel_slow, 0, sizeof(sel_slow));
    sel_beam = 0;
    sel_candidate = 0;
    sel_count = 0;
    sel_dwell = SEL_MIN_DWELL;
    memset(mm_x1, 0, sizeof(mm_x1));
    memset(mm_y1, 0, sizeof(mm_y1));
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

static uint16_t beam_used;

static void frontend(int32_t *mics, int32_t samples[2][SIM_FRAME], const doa_result_t *d, int wake, int conversation,
                     float vnr)
{
    const int beam = (cfg.mode == MIC_MODE_BEAM);
    mic_frontend_set_steering(&fe, cfg.policy, conversation);
    mic_frontend_update(&fe, cfg.mode, d);
    if (wake && !(beam && cfg.idle_select == 2)) {
        (void)mic_frontend_wake_word(&fe, cfg.mode);
    }
    if (beam && (cfg.beam_kind || cfg.idle_select)) {
        sdb_analyse(&sdb, (const int32_t (*)[SDB_HOP])mics);
    }
    if (beam && cfg.idle_select) {
        idle_select_update(vnr);
        /* Outside a conversation the beam follows the selection; a wake word steers it as before */
        const unsigned sel_az = sel_beam * 360 / SDB_MULTI;
        if (!conversation && !wake && fe.beam.azimuth_deg != sel_az) {
            beamformer_steer(&fe.beam, sel_az);
        }
    }
    if (frame_no < replay_frames) {
        if (beam && fe.beam.azimuth_deg != replay_beam[frame_no]) {
            beamformer_steer(&fe.beam, replay_beam[frame_no]);
        }
        if (cfg.mode == MIC_MODE_AUTO_PAIR) {
            static const uint8_t opposite[4] = { MIC_IDX_SOUTH, MIC_IDX_NORTH, MIC_IDX_WEST, MIC_IDX_EAST };
            for (uint8_t m = 0; m < 4; m++) {
                if (replay_ch0[frame_no] == MIC_MASK(m)) {
                    fe.ch0 = m;
                    fe.ch1 = opposite[m];
                }
            }
        }
    }
    frame_no++;
    mic_frontend_process(&fe, cfg.mode, mics, &samples[0][0], SIM_FRAME);
    beam_used = mic_frontend_get_beam_azimuth(&fe, cfg.mode);
    if (beam && cfg.beam_kind) {
        sdb_steer(&sdb, beam_used);
        sdb_beams(&sdb, samples);
    }
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
    if (cfg.mic_mismatch) {
        apply_mic_mismatch(mics);
    }

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
        out->doa = *doa_step(&mics[0][0], playback, vnr);
        out->doa_gated = playback;
        frontend(&mics[0][0], samples, &out->doa, wake, conversation, vnr);
        if (cfg.hpf) {
            mic_hpf_process(&hpf_lo, &samples[0][0], SIM_FRAME);
        }
        out->aec_in_energy = audio_mean_square_s32(&samples[0][0], 2 * SIM_FRAME);
        aec_process_frame(&aec, samples, NULL, samples, ref, NULL);
        out->aec_out_energy = audio_mean_square_s32(&samples[0][0], 2 * SIM_FRAME);
    } else {
        /* stage_capture(), stage_aec(), stage_frontend_after_aec() */
        if (!cfg.doa_post_aec) {
            out->doa = *doa_step(&mics[0][0], playback, vnr);
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
            out->doa = *doa_step(&mics[0][0], echo, vnr);
            out->doa_gated = echo;
        }
        frontend(&mics[0][0], samples, &out->doa, wake, conversation, vnr);
    }
    mic_apply_gain_q16(&samples[0][0], 2 * SIM_FRAME, mic_gain_db_to_q16(cfg.mic_gain_db));

    out->beam_azimuth = beam_used;
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
