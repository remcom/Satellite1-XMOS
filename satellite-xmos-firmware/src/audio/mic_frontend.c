#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "xmath/xmath.h"

#include "audio/mic_frontend.h"

/* vect_s32_energy() cannot saturate its accumulators for blocks up to this length */
#define ENERGY_BLOCK_MAX 1024

#define MIC_FRONTEND_PI 3.14159265358979f
#define ALL_MICS_MASK   ((uint8_t)((1u << MIC_FRONTEND_MIC_COUNT) - 1))

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

void mic_frontend_init(mic_frontend_state_t *state)
{
    memset(state, 0, sizeof(*state));
    state->ch0 = MIC_IDX_EAST;
    state->ch1 = MIC_IDX_WEST;
    state->candidate = MIC_IDX_EAST;
    beamformer_init(&state->beam, MIC_FRONTEND_BEAM_INITIAL_DEG);
}

static void set_pair(mic_frontend_state_t *state, uint8_t ch0)
{
    state->ch0 = ch0;
    state->ch1 = opposite_mic[ch0];
    state->candidate = ch0;
    state->candidate_frames = 0;
}

static unsigned angle_between(unsigned a_deg, unsigned b_deg)
{
    const unsigned d = (a_deg > b_deg ? a_deg - b_deg : b_deg - a_deg) % 360;
    return d > 180 ? 360 - d : d;
}

static void reset_beam_tracking(mic_frontend_state_t *state)
{
    state->beam_away_frames = 0;
    state->beam_away_x = 0.0f;
    state->beam_away_y = 0.0f;
}

/*
 * Re-steers once the DOA has stayed outside the hold angle for long enough, to
 * the mean of those DOA results: a talker between two DOA outcomes still gets
 * a beam, and single outliers barely move it.
 */
static void update_beam(mic_frontend_state_t *state, const doa_result_t *doa)
{
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
    if (state->frames_since_switch < MIC_FRONTEND_AUTO_MIN_DWELL_FRAMES) {
        state->frames_since_switch++;
    }
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
        update_beam(state, doa);
        return;
    default:
        return;
    }

    if (!doa->updated) {
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
