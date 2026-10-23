#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "audio/mic_frontend.h"

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
}

static void set_pair(mic_frontend_state_t *state, uint8_t ch0)
{
    state->ch0 = ch0;
    state->ch1 = opposite_mic[ch0];
    state->candidate = ch0;
    state->candidate_frames = 0;
}

void mic_frontend_update(mic_frontend_state_t *state,
                         mic_mode_t mode,
                         const doa_result_t *doa)
{
    if (state->frames_since_switch < MIC_FRONTEND_AUTO_MIN_DWELL_FRAMES) {
        state->frames_since_switch++;
    }

    switch (mode) {
    case MIC_MODE_PAIR_EW:
        set_pair(state, MIC_IDX_EAST);
        return;
    case MIC_MODE_PAIR_NS:
        set_pair(state, MIC_IDX_NORTH);
        return;
    case MIC_MODE_AUTO_PAIR:
        break;
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

void mic_frontend_process(const mic_frontend_state_t *state,
                          mic_mode_t mode,
                          const int32_t *raw,
                          int32_t *out,
                          size_t frame_count)
{
    int32_t *out0 = &out[0];
    int32_t *out1 = &out[frame_count];

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
    uint8_t ch0;
    uint8_t ch1;
    pair_for_mode(state, mode, &ch0, &ch1);
    *ch0_mask = MIC_MASK(ch0);
    *ch1_mask = MIC_MASK(ch1);
}

int32_t mic_gain_db_to_q16(unsigned gain_db)
{
    return (int32_t)(65536.0f * powf(10.0f, (float)gain_db / 20.0f) + 0.5f);
}

void mic_apply_gain_q16(int32_t *samples, size_t count, int32_t gain_q16)
{
    for (size_t i = 0; i < count; i++) {
        int64_t v = ((int64_t)samples[i] * gain_q16 + (1 << 15)) >> 16;
        if (v > INT32_MAX) {
            v = INT32_MAX;
        } else if (v < INT32_MIN) {
            v = INT32_MIN;
        }
        samples[i] = (int32_t)v;
    }
}
