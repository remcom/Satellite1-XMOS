#pragma once

#include <stddef.h>
#include <stdint.h>

#include "xmath/xmath.h"

#include "audio/beamformer.h"
#include "audio/doa.h"

/*
 * Reduces the four Satellite1 microphones to the two channels the
 * AEC/IC/NS/AGC pipeline is built for. Channel 0 is the IC target, channel 1
 * its noise reference.
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
    MIC_MODE_BEAM      = 4, /* DOA steered: beam at the talker, blocking beam (audio/beamformer.h) */
    MIC_MODE_COUNT
} mic_mode_t;

/* AUTO_PAIR, BEAM: the DOA must point somewhere new for this many speech frames ... */
#define MIC_FRONTEND_AUTO_STABLE_FRAMES     33  /* ~0.5 s of speech at 15 ms/frame */
/* ... and the previous switch must be at least this old before switching again. */
#define MIC_FRONTEND_AUTO_MIN_DWELL_FRAMES  200 /* ~3 s */
/* AUTO_PAIR: the DOA must lie within this angle of a mic's axis to select it.
 * Less than 45 degrees, so there is a dead band between neighbouring mics. */
#define MIC_FRONTEND_AUTO_SECTOR_DEG        35
/*
 * BEAM: DOA results within this angle of the steering keep the beams where
 * they are. A talker this far off the steering reaches the blocking beam
 * ~23 dB (0.15..4 kHz) below a source behind the device. A tighter angle
 * re-steers, and so re-adapts the AEC and IC, more often.
 */
#define MIC_FRONTEND_BEAM_HOLD_DEG          15
/*
 * BEAM: the DOA results outside the hold angle must agree at least this well
 * (length of the mean of their unit vectors, 1 = all equal) to steer to their
 * mean; otherwise they are dropped. Two talkers taking turns would put the
 * mean between them. 0.8 still accepts two DOA outcomes ~70 degrees apart.
 */
#define MIC_FRONTEND_BEAM_MIN_CLUSTER       0.8f
/* BEAM: steering direction until the DOA moves it */
#define MIC_FRONTEND_BEAM_INITIAL_DEG       0
/* mic_frontend_get_beam_azimuth() outside MIC_MODE_BEAM */
#define MIC_FRONTEND_NO_BEAM                0xFFFF

typedef struct {
    uint8_t ch0;                 /* mic_idx_t feeding channel 0 (the IC target) */
    uint8_t ch1;                 /* mic_idx_t feeding channel 1 */
    uint8_t mode;                /* mic_mode_t of the last update, to spot entering BEAM */
    uint8_t candidate;           /* AUTO_PAIR: mic the DOA currently points at */
    uint32_t candidate_frames;   /* AUTO_PAIR: speech frames the candidate held */
    uint32_t frames_since_switch;
    uint32_t beam_away_frames;   /* BEAM: speech frames with the DOA outside the hold angle */
    float beam_away_x;           /* BEAM: sum of their unit vectors, for the mean direction */
    float beam_away_y;
    beamformer_t beam;
} mic_frontend_state_t;

void mic_frontend_init(mic_frontend_state_t *state);

/**
 * Advances the AUTO_PAIR selection or the BEAM steering with this frame's DOA
 * result. Call once per frame before mic_frontend_process(); it has no effect
 * in the other modes.
 *
 * Entering BEAM points the beams at the DOA result straight away if it is
 * valid, without a crossfade, and does not wait out the dwell of an earlier
 * switch: the mode change makes the AEC and IC re-adapt anyway.
 */
void mic_frontend_update(mic_frontend_state_t *state,
                         mic_mode_t mode,
                         const doa_result_t *doa);

/**
 * Writes the two pipeline channels for the given mode. Outside BEAM it still
 * keeps the beamformer's filter history up to date.
 *
 * \param raw   MIC_FRONTEND_MIC_COUNT channels of frame_count samples
 * \param out   2 channels of frame_count samples
 */
void mic_frontend_process(mic_frontend_state_t *state,
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

/** Steering azimuth in degrees in MIC_MODE_BEAM, else MIC_FRONTEND_NO_BEAM. */
uint16_t mic_frontend_get_beam_azimuth(const mic_frontend_state_t *state, mic_mode_t mode);

/** Linear Q16 gain for a gain in dB. */
int32_t mic_gain_db_to_q16(unsigned gain_db);

/**
 * Multiplies samples by a Q16 gain on the VPU, saturating symmetrically to
 * +-INT32_MAX. samples must be word aligned.
 */
void mic_apply_gain_q16(int32_t *samples, size_t count, int32_t gain_q16);

/**
 * Mean square of samples, full scale = 1.0. Uses the VPU with the block's
 * headroom shifted out, so quiet signals keep their precision. samples must
 * be word aligned.
 */
float audio_mean_square_s32(const int32_t *samples, size_t count);

/**
 * Energies of `channels` consecutive blocks of `count` samples, in one pass
 * each. Returns the mean square over all blocks (as audio_mean_square_s32()
 * of the whole buffer) and sets *max_energy to the largest block energy,
 * exactly as aec_calc_max_input_energy() computes it. samples must be word
 * aligned.
 */
float audio_block_energies_s32(const int32_t *samples, size_t channels, size_t count,
                               float_s32_t *max_energy);
