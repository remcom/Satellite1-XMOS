#pragma once

#include <stddef.h>
#include <stdint.h>

#include "audio/doa.h"

/*
 * Reduces the four Satellite1 microphones to the two channels the
 * AEC/IC/NS/AGC pipeline is built for.
 *
 * Raw channel order follows MIC_ARRAY_CONFIG_INPUT_MAPPING in SATELLITE1.cmake.
 */
#define MIC_FRONTEND_MIC_COUNT  4

typedef enum {
    MIC_IDX_NORTH = 0,
    MIC_IDX_SOUTH = 1,
    MIC_IDX_EAST  = 2,
    MIC_IDX_WEST  = 3,
} mic_idx_t;

#define MIC_MASK(idx) ((uint8_t)(1u << (idx)))

typedef enum {
    MIC_MODE_PAIR_EW   = 0, /* East, West (pre-4-mic behaviour) */
    MIC_MODE_PAIR_NS   = 1, /* North, South */
    MIC_MODE_AVERAGE   = 2, /* (North+East)/2, (South+West)/2 */
    MIC_MODE_AUTO_PAIR = 3, /* DOA steered: mic facing the talker, opposite mic */
    MIC_MODE_COUNT
} mic_mode_t;

/* AUTO_PAIR: the DOA must point at a new mic for this many speech frames ... */
#define MIC_FRONTEND_AUTO_STABLE_FRAMES     33  /* ~0.5 s of speech at 15 ms/frame */
/* ... and the previous switch must be at least this old before switching again. */
#define MIC_FRONTEND_AUTO_MIN_DWELL_FRAMES  200 /* ~3 s */
/* AUTO_PAIR: the DOA must lie within this angle of a mic's axis to select it.
 * Less than 45 degrees, so there is a dead band between neighbouring mics. */
#define MIC_FRONTEND_AUTO_SECTOR_DEG        35

typedef struct {
    uint8_t ch0;                 /* mic_idx_t feeding channel 0 (the IC target) */
    uint8_t ch1;                 /* mic_idx_t feeding channel 1 */
    uint8_t candidate;           /* AUTO_PAIR: mic the DOA currently points at */
    uint32_t candidate_frames;   /* AUTO_PAIR: speech frames the candidate held */
    uint32_t frames_since_switch;
} mic_frontend_state_t;

void mic_frontend_init(mic_frontend_state_t *state);

/**
 * Advances the AUTO_PAIR selection with this frame's DOA result. Call once per
 * frame before mic_frontend_process(); it has no effect in the other modes.
 */
void mic_frontend_update(mic_frontend_state_t *state,
                         mic_mode_t mode,
                         const doa_result_t *doa);

/**
 * Writes the two pipeline channels for the given mode.
 *
 * \param raw   MIC_FRONTEND_MIC_COUNT channels of frame_count samples
 * \param out   2 channels of frame_count samples
 */
void mic_frontend_process(const mic_frontend_state_t *state,
                          mic_mode_t mode,
                          const int32_t *raw,
                          int32_t *out,
                          size_t frame_count);

/**
 * Bitmasks (MIC_MASK) of the mics that feed channel 0 and channel 1.
 */
void mic_frontend_get_masks(const mic_frontend_state_t *state,
                            mic_mode_t mode,
                            uint8_t *ch0_mask,
                            uint8_t *ch1_mask);

/** Linear Q16 gain for a gain in dB. */
int32_t mic_gain_db_to_q16(unsigned gain_db);

/** Multiplies samples by a Q16 gain, saturating to the int32 range. */
void mic_apply_gain_q16(int32_t *samples, size_t count, int32_t gain_q16);
