#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "xmath/xmath.h"

#include "audio/mic_frontend.h"

/* vect_s32_energy() cannot saturate its accumulators for blocks up to this length */
#define ENERGY_BLOCK_MAX 1024

#define MIC_FRONTEND_PI 3.14159265358979f
#define ALL_MICS_MASK   ((uint8_t)((1u << MIC_FRONTEND_MIC_COUNT) - 1))
#define DOA_UNIT_Q14    16384.0f

static const uint8_t opposite_mic[MIC_FRONTEND_MIC_COUNT] = {
    [MIC_IDX_NORTH] = MIC_IDX_SOUTH,
    [MIC_IDX_SOUTH] = MIC_IDX_NORTH,
    [MIC_IDX_EAST]  = MIC_IDX_WEST,
    [MIC_IDX_WEST]  = MIC_IDX_EAST,
};

/* Mic on the axis of each 90 degree sector, sectors centred on 0, 90, 180, 270 degrees. */
static const uint8_t sector_mic[4] = {
    MIC_IDX_NORTH, MIC_IDX_EAST, MIC_IDX_SOUTH, MIC_IDX_WEST,
};

static unsigned angle_between(unsigned a_deg, unsigned b_deg)
{
    const unsigned d = (a_deg > b_deg ? a_deg - b_deg : b_deg - a_deg) % 360;
    return d > 180 ? 360 - d : d;
}

void mic_frontend_init(mic_frontend_state_t *state)
{
    memset(state, 0, sizeof(*state));
    state->ch0 = MIC_IDX_EAST;
    state->ch1 = MIC_IDX_WEST;
    state->candidate = MIC_IDX_EAST;
    state->mode = MIC_MODE_COUNT;
    state->steer_policy = STEER_POLICY_FOLLOW_SPEECH;
    state->wake_azimuth_deg = MIC_FRONTEND_NO_BEAM;
    beamformer_init(&state->beam, MIC_FRONTEND_BEAM_INITIAL_DEG);
}

void mic_frontend_set_steering(mic_frontend_state_t *state,
                               steer_policy_t policy,
                               int conversation_active)
{
    state->steer_policy = (policy < STEER_POLICY_COUNT) ? (uint8_t)policy : STEER_POLICY_FOLLOW_SPEECH;
    if (!conversation_active) {
        state->conversation = 0;
        state->conversation_frames = 0;
        return;
    }
    if (state->conversation_frames < MIC_FRONTEND_CONVERSATION_MAX_FRAMES) {
        state->conversation_frames++;
    }
    state->conversation = state->conversation_frames < MIC_FRONTEND_CONVERSATION_MAX_FRAMES;
}

/* Only speech under STEER_POLICY_FOLLOW_SPEECH, outside a conversation, moves the steering */
static int speech_steers(const mic_frontend_state_t *state)
{
    return state->steer_policy == STEER_POLICY_FOLLOW_SPEECH && !state->conversation;
}

static void record_doa(mic_frontend_state_t *state, const doa_result_t *doa)
{
    state->frame_count++;
    if (!doa->updated) {
        return;
    }
    mic_frontend_doa_entry_t *e = &state->doa_history[state->doa_history_next];
    const float a = (float)(doa->azimuth_deg % 360) * (MIC_FRONTEND_PI / 180.0f);
    e->frame = state->frame_count;
    e->x_q14 = (int16_t)lrintf(cosf(a) * DOA_UNIT_Q14);
    e->y_q14 = (int16_t)lrintf(sinf(a) * DOA_UNIT_Q14);
    e->confidence = doa->confidence;
    state->doa_history_next = (state->doa_history_next + 1) % MIC_FRONTEND_WAKE_HISTORY;
    if (state->doa_history_len < MIC_FRONTEND_WAKE_HISTORY) {
        state->doa_history_len++;
    }

    /* The conversation is the talker we already know about, so only idle speech counts */
    if (!state->conversation) {
        const float decay = 1.0f - 1.0f / (float)MIC_FRONTEND_IDLE_DOA_FRAMES;
        for (unsigned b = 0; b < MIC_FRONTEND_IDLE_DOA_BINS; b++) {
            state->idle_doa[b] *= decay;
        }
        const unsigned bin = (doa->azimuth_deg % 360) * MIC_FRONTEND_IDLE_DOA_BINS / 360;
        state->idle_doa[bin] += 1.0f - decay;
    }
}

/* Share of the idle speech that came from within MIC_FRONTEND_WAKE_CLUSTER_DEG of azimuth_deg */
static float idle_share(const mic_frontend_state_t *state, unsigned azimuth_deg)
{
    const unsigned bin_deg = 360 / MIC_FRONTEND_IDLE_DOA_BINS;
    float near = 0.0f;
    float total = 0.0f;
    for (unsigned b = 0; b < MIC_FRONTEND_IDLE_DOA_BINS; b++) {
        total += state->idle_doa[b];
        if (angle_between(b * bin_deg + bin_deg / 2, azimuth_deg) <= MIC_FRONTEND_WAKE_CLUSTER_DEG) {
            near += state->idle_doa[b];
        }
    }
    return (total > 0.0f) ? near / total : 0.0f;
}

static void set_pair(mic_frontend_state_t *state, uint8_t ch0)
{
    state->ch0 = ch0;
    state->ch1 = opposite_mic[ch0];
    state->candidate = ch0;
    state->candidate_frames = 0;
}

static void reset_beam_tracking(mic_frontend_state_t *state)
{
    state->beam_away_frames = 0;
    state->beam_away_x = 0.0f;
    state->beam_away_y = 0.0f;
}

static void enter_beam(mic_frontend_state_t *state, const doa_result_t *doa)
{
    if (state->steer_policy == STEER_POLICY_WAKE_WORD &&
        state->wake_azimuth_deg != MIC_FRONTEND_NO_BEAM) {
        beamformer_point(&state->beam, state->wake_azimuth_deg);
    } else if (doa->valid && state->steer_policy != STEER_POLICY_FIXED) {
        beamformer_point(&state->beam, doa->azimuth_deg);
    }
    state->frames_since_switch = MIC_FRONTEND_AUTO_MIN_DWELL_FRAMES;
}

/*
 * Re-steers once the DOA has stayed outside the hold angle for long enough, to
 * the mean of those DOA results: a talker between two DOA outcomes still gets
 * a beam, and single outliers barely move it. Results spread too widely for
 * their mean to mean anything are dropped instead.
 */
static void update_beam(mic_frontend_state_t *state, const doa_result_t *doa)
{
    if (!speech_steers(state)) {
        reset_beam_tracking(state);
        return;
    }
    if (!doa->updated) {
        return;
    }
    if (angle_between(doa->azimuth_deg % 360, state->beam.azimuth_deg) <= MIC_FRONTEND_BEAM_HOLD_DEG) {
        reset_beam_tracking(state);
        return;
    }

    const float az = (float)doa->azimuth_deg * (MIC_FRONTEND_PI / 180.0f);
    state->beam_away_x += cosf(az);
    state->beam_away_y += sinf(az);
    state->beam_away_frames++;

    if (state->beam_away_frames >= MIC_FRONTEND_AUTO_STABLE_FRAMES &&
        state->frames_since_switch >= MIC_FRONTEND_AUTO_MIN_DWELL_FRAMES) {
        const float spread_limit = MIC_FRONTEND_BEAM_MIN_CLUSTER * (float)state->beam_away_frames;
        if (hypotf(state->beam_away_x, state->beam_away_y) < spread_limit) {
            reset_beam_tracking(state);
            return;
        }
        float mean = atan2f(state->beam_away_y, state->beam_away_x) * (180.0f / MIC_FRONTEND_PI);
        if (mean < 0.0f) {
            mean += 360.0f;
        }
        beamformer_steer(&state->beam, (unsigned)lrintf(mean) % 360);
        reset_beam_tracking(state);
        state->frames_since_switch = 0;
    }
}

void mic_frontend_update(mic_frontend_state_t *state,
                         mic_mode_t mode,
                         const doa_result_t *doa)
{
    record_doa(state, doa);
    if (state->frames_since_switch < MIC_FRONTEND_AUTO_MIN_DWELL_FRAMES) {
        state->frames_since_switch++;
    }
    const int entering_beam = (mode == MIC_MODE_BEAM && state->mode != MIC_MODE_BEAM);
    state->mode = (uint8_t)mode;
    if (mode != MIC_MODE_BEAM) {
        reset_beam_tracking(state);
    }

    switch (mode) {
    case MIC_MODE_PAIR_EW:
    case MIC_MODE_AVERAGE:  /* so a later switch to AUTO_PAIR starts from E/W */
        set_pair(state, MIC_IDX_EAST);
        return;
    case MIC_MODE_PAIR_NS:
        set_pair(state, MIC_IDX_NORTH);
        return;
    case MIC_MODE_AUTO_PAIR:
        break;
    case MIC_MODE_BEAM:
        if (entering_beam) {
            enter_beam(state, doa);
        }
        update_beam(state, doa);
        return;
    default:
        return;
    }

    if (!doa->updated) {
        return;
    }
    if (!speech_steers(state)) {
        state->candidate = state->ch0;
        state->candidate_frames = 0;
        return;
    }

    const unsigned azimuth = doa->azimuth_deg % 360;
    const unsigned sector = ((azimuth + 45) / 90) % 4;
    int diff = abs((int)azimuth - (int)(sector * 90));
    if (diff > 180) {
        diff = 360 - diff;
    }
    if (diff > MIC_FRONTEND_AUTO_SECTOR_DEG) {
        return; /* between two mics: keep the current choice */
    }

    const uint8_t mic = sector_mic[sector];
    if (mic == state->ch0) {
        state->candidate = mic;
        state->candidate_frames = 0;
        return;
    }
    if (mic != state->candidate) {
        state->candidate = mic;
        state->candidate_frames = 0;
    }
    state->candidate_frames++;

    if (state->candidate_frames >= MIC_FRONTEND_AUTO_STABLE_FRAMES &&
        state->frames_since_switch >= MIC_FRONTEND_AUTO_MIN_DWELL_FRAMES) {
        set_pair(state, mic);
        state->frames_since_switch = 0;
    }
}

static unsigned vector_azimuth(float x, float y)
{
    float deg = atan2f(y, x) * (180.0f / MIC_FRONTEND_PI);
    if (deg < 0.0f) {
        deg += 360.0f;
    }
    return (unsigned)lrintf(deg) % 360;
}

/*
 * Direction of the wake word: the confidence weighted mean of a group of
 * recent DOA results within MIC_FRONTEND_WAKE_CLUSTER_DEG of one of them.
 * Speech from elsewhere in the window (a TV before and after the wake word,
 * the start of the command) forms groups of its own; the best ranked one
 * wins, see MIC_FRONTEND_WAKE_NOVELTY. Results spread too widely to have a
 * group with MIC_FRONTEND_WAKE_MIN_SHARE of them give no direction.
 */
static int wake_direction(const mic_frontend_state_t *state, unsigned *azimuth_deg)
{
    /* Entries closer than the cluster angle have unit vectors with a larger dot product */
    const float min_dot = cosf((float)MIC_FRONTEND_WAKE_CLUSTER_DEG * (MIC_FRONTEND_PI / 180.0f)) *
                          DOA_UNIT_Q14 * DOA_UNIT_Q14;
    const mic_frontend_doa_entry_t *h = state->doa_history;
    float total = 0.0f;
    for (unsigned i = 0; i < state->doa_history_len; i++) {
        if (state->frame_count - h[i].frame < MIC_FRONTEND_WAKE_WINDOW_FRAMES) {
            total += (float)h[i].confidence + 1.0f;
        }
    }

    float best_score = 0.0f;
    unsigned best_azimuth = 0;
    for (unsigned i = 0; i < state->doa_history_len; i++) {
        if (state->frame_count - h[i].frame >= MIC_FRONTEND_WAKE_WINDOW_FRAMES) {
            continue;
        }
        unsigned count = 0;
        float w = 0.0f;
        float x = 0.0f;
        float y = 0.0f;
        for (unsigned j = 0; j < state->doa_history_len; j++) {
            if (state->frame_count - h[j].frame >= MIC_FRONTEND_WAKE_WINDOW_FRAMES) {
                continue;
            }
            const float dot = (float)h[i].x_q14 * (float)h[j].x_q14 + (float)h[i].y_q14 * (float)h[j].y_q14;
            if (dot >= min_dot) {
                const float wj = (float)h[j].confidence + 1.0f;
                count++;
                w += wj;
                x += wj * (float)h[j].x_q14;
                y += wj * (float)h[j].y_q14;
            }
        }
        if (count < MIC_FRONTEND_WAKE_MIN_RESULTS || w < MIC_FRONTEND_WAKE_MIN_SHARE * total) {
            continue;
        }
        const unsigned azimuth = vector_azimuth(x, y);
        const float score = w * (1.0f - MIC_FRONTEND_WAKE_NOVELTY * idle_share(state, azimuth));
        if (score > best_score) {
            best_score = score;
            best_azimuth = azimuth;
        }
    }
    if (best_score <= 0.0f) {
        return 0;
    }
    *azimuth_deg = best_azimuth;
    return 1;
}

/* Mic closest to azimuth_deg */
static uint8_t facing_mic(unsigned azimuth_deg)
{
    return sector_mic[((azimuth_deg % 360 + 45) / 90) % 4];
}

int mic_frontend_wake_word(mic_frontend_state_t *state, mic_mode_t mode)
{
    unsigned azimuth;
    if (!wake_direction(state, &azimuth)) {
        return 0;
    }
    state->wake_azimuth_deg = (uint16_t)azimuth;
    if (state->steer_policy == STEER_POLICY_FIXED) {
        return 1;
    }

    if (mode == MIC_MODE_BEAM) {
        reset_beam_tracking(state);
        /* A small correction is not worth making the AEC and IC re-adapt */
        if (angle_between(azimuth, state->beam.azimuth_deg) > MIC_FRONTEND_BEAM_HOLD_DEG) {
            beamformer_steer(&state->beam, azimuth);
            state->frames_since_switch = 0;
        }
    } else if (mode == MIC_MODE_AUTO_PAIR) {
        const uint8_t mic = facing_mic(azimuth);
        if (mic != state->ch0) {
            set_pair(state, mic);
            state->frames_since_switch = 0;
        }
    }
    return 1;
}

static void pair_for_mode(const mic_frontend_state_t *state, mic_mode_t mode,
                          uint8_t *ch0, uint8_t *ch1)
{
    switch (mode) {
    case MIC_MODE_PAIR_NS:
        *ch0 = MIC_IDX_NORTH;
        *ch1 = MIC_IDX_SOUTH;
        break;
    case MIC_MODE_AUTO_PAIR:
        *ch0 = state->ch0;
        *ch1 = state->ch1;
        break;
    case MIC_MODE_PAIR_EW:
    default:
        *ch0 = MIC_IDX_EAST;
        *ch1 = MIC_IDX_WEST;
        break;
    }
}

void mic_frontend_process(mic_frontend_state_t *state,
                          mic_mode_t mode,
                          const int32_t *raw,
                          int32_t *out,
                          size_t frame_count)
{
    int32_t *out0 = &out[0];
    int32_t *out1 = &out[frame_count];

    if (mode == MIC_MODE_BEAM) {
        beamformer_process(&state->beam, raw, out, frame_count);
        return;
    }
    beamformer_feed(&state->beam, raw, frame_count);

    if (mode == MIC_MODE_AVERAGE) {
        /*
         * Adjacent mics are averaged so the two virtual mics stay apart (NE and
         * SW of the centre). Averaging opposite mics would put both at the
         * centre and leave nothing for the IC stage to work with.
         */
        const int32_t *n = &raw[MIC_IDX_NORTH * frame_count];
        const int32_t *s = &raw[MIC_IDX_SOUTH * frame_count];
        const int32_t *e = &raw[MIC_IDX_EAST * frame_count];
        const int32_t *w = &raw[MIC_IDX_WEST * frame_count];
        for (size_t i = 0; i < frame_count; i++) {
            out0[i] = (n[i] >> 1) + (e[i] >> 1);
            out1[i] = (s[i] >> 1) + (w[i] >> 1);
        }
        return;
    }

    uint8_t ch0;
    uint8_t ch1;
    pair_for_mode(state, mode, &ch0, &ch1);
    memcpy(out0, &raw[ch0 * frame_count], frame_count * sizeof(int32_t));
    memcpy(out1, &raw[ch1 * frame_count], frame_count * sizeof(int32_t));
}

void mic_frontend_get_masks(const mic_frontend_state_t *state,
                            mic_mode_t mode,
                            uint8_t *ch0_mask,
                            uint8_t *ch1_mask)
{
    if (mode == MIC_MODE_AVERAGE) {
        *ch0_mask = MIC_MASK(MIC_IDX_NORTH) | MIC_MASK(MIC_IDX_EAST);
        *ch1_mask = MIC_MASK(MIC_IDX_SOUTH) | MIC_MASK(MIC_IDX_WEST);
        return;
    }
    if (mode == MIC_MODE_BEAM) {
        *ch0_mask = ALL_MICS_MASK;
        *ch1_mask = ALL_MICS_MASK;
        return;
    }
    uint8_t ch0;
    uint8_t ch1;
    pair_for_mode(state, mode, &ch0, &ch1);
    *ch0_mask = MIC_MASK(ch0);
    *ch1_mask = MIC_MASK(ch1);
}

uint16_t mic_frontend_get_beam_azimuth(const mic_frontend_state_t *state, mic_mode_t mode)
{
    return (mode == MIC_MODE_BEAM) ? state->beam.azimuth_deg : MIC_FRONTEND_NO_BEAM;
}

#define HPF_Q28 268435456.0f

void mic_hpf_init(mic_hpf_t *hpf)
{
    /* Bilinear transform of a second order Butterworth high-pass */
    const float k = tanf(MIC_FRONTEND_PI * (float)MIC_HPF_CUTOFF_HZ / (float)MIC_HPF_SAMPLE_RATE);
    const float q = 0.70710678f;
    const float norm = 1.0f / (1.0f + k / q + k * k);
    hpf->b0 = (int32_t)lrintf(norm * HPF_Q28);
    hpf->b1 = -2 * hpf->b0;
    hpf->b2 = hpf->b0;
    hpf->a1 = (int32_t)lrintf(2.0f * (k * k - 1.0f) * norm * HPF_Q28);
    hpf->a2 = (int32_t)lrintf((1.0f - k / q + k * k) * norm * HPF_Q28);
    mic_hpf_reset(hpf);
}

void mic_hpf_reset(mic_hpf_t *hpf)
{
    memset(hpf->x1, 0, sizeof(hpf->x1));
    memset(hpf->x2, 0, sizeof(hpf->x2));
    memset(hpf->y1, 0, sizeof(hpf->y1));
    memset(hpf->y2, 0, sizeof(hpf->y2));
}

void mic_hpf_process(mic_hpf_t *hpf, int32_t *samples, size_t frame_count)
{
    for (unsigned ch = 0; ch < MIC_HPF_CHANNELS; ch++) {
        int32_t *x = &samples[ch * frame_count];
        int32_t x1 = hpf->x1[ch];
        int32_t x2 = hpf->x2[ch];
        int32_t y1 = hpf->y1[ch];
        int32_t y2 = hpf->y2[ch];
        for (size_t i = 0; i < frame_count; i++) {
            /*
             * Direct form I. The Q28 coefficients add up to less than 8 in
             * absolute value, so the 64-bit sum of Q31 products cannot overflow.
             */
            const int32_t x0 = x[i];
            int64_t acc = (int64_t)hpf->b0 * x0 + (int64_t)hpf->b1 * x1 + (int64_t)hpf->b2 * x2
                        - (int64_t)hpf->a1 * y1 - (int64_t)hpf->a2 * y2;
            acc = (acc + (1 << 27)) >> 28;
            if (acc > INT32_MAX) {
                acc = INT32_MAX;
            } else if (acc < -INT32_MAX) {
                acc = -INT32_MAX;
            }
            x2 = x1;
            x1 = x0;
            y2 = y1;
            y1 = (int32_t)acc;
            x[i] = y1;
        }
        hpf->x1[ch] = x1;
        hpf->x2[ch] = x2;
        hpf->y1[ch] = y1;
        hpf->y2[ch] = y2;
    }
}

int32_t mic_gain_db_to_q16(unsigned gain_db)
{
    return (int32_t)(65536.0f * powf(10.0f, (float)gain_db / 20.0f) + 0.5f);
}

void mic_apply_gain_q16(int32_t *samples, size_t count, int32_t gain_q16)
{
    if (gain_q16 <= 0) {
        memset(samples, 0, count * sizeof(int32_t));
        return;
    }
    /*
     * gain = scale * 2^(shl - 30) with scale < 2^31: a Q30 scale, then a
     * saturating left shift for gains above 2.
     */
    const unsigned bits = 32 - (unsigned)__builtin_clz((uint32_t)gain_q16);
    const unsigned shl = (bits > 17) ? bits - 17 : 0;
    const int32_t scale = gain_q16 << (14 - shl);
    vect_s32_scale(samples, samples, count, scale, 0, 0);
    if (shl > 0) {
        vect_s32_shl(samples, samples, count, (left_shift_t)shl);
    }
}

float audio_mean_square_s32(const int32_t *samples, size_t count)
{
    if (count == 0) {
        return 0.0f;
    }
    float sum = 0.0f;
    for (size_t done = 0; done < count; done += ENERGY_BLOCK_MAX) {
        const size_t len = (count - done < ENERGY_BLOCK_MAX) ? count - done : ENERGY_BLOCK_MAX;
        const headroom_t hr = vect_s32_headroom(&samples[done], len);
        /* sum of (x * 2^hr)^2 * 2^-30, with x in units of 2^-31 full scale */
        const int64_t e = vect_s32_energy(&samples[done], len, -(right_shift_t)hr);
        sum += ldexpf((float)e, -32 - 2 * (int)hr);
    }
    return sum / (float)count;
}

float audio_block_energies_s32(const int32_t *samples, size_t channels, size_t count,
                               float_s32_t *max_energy)
{
    float sum = 0.0f;
    *max_energy = (float_s32_t){0, 0};
    for (size_t ch = 0; ch < channels; ch++) {
        bfp_s32_t block;
        /* Q31 input, as in aec_calc_max_input_energy() */
        bfp_s32_init(&block, (int32_t *)&samples[ch * count], -31, count, 1);
        const float_s64_t e = bfp_s32_energy(&block);
        const float_s32_t e32 = float_s64_to_float_s32(e);
        if (ch == 0 || float_s32_gt(e32, *max_energy)) {
            *max_energy = e32;
        }
        sum += ldexpf((float)e.mant, e.exp);
    }
    return (channels * count > 0) ? sum / (float)(channels * count) : 0.0f;
}
