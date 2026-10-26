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

/*
 * What may move the BEAM steering and the AUTO_PAIR selection. A wake word
 * (mic_frontend_wake_word()) steers at once in FOLLOW_SPEECH and WAKE_WORD.
 */
typedef enum {
    STEER_POLICY_FOLLOW_SPEECH = 0, /* any speech, after MIC_FRONTEND_AUTO_STABLE_FRAMES */
    STEER_POLICY_WAKE_WORD     = 1, /* only wake words: a TV or radio cannot pull the beam */
    STEER_POLICY_FIXED         = 2, /* nothing, the steering stays where it is */
    STEER_POLICY_COUNT
} steer_policy_t;

/*
 * Wake word steering: DOA results of the last MIC_FRONTEND_WAKE_WINDOW_FRAMES
 * are kept. A wake word is ~0.8 s long and is reported a few hundred ms after
 * it ended (detection, I2S, resampler and SPI latency).
 */
#define MIC_FRONTEND_WAKE_WINDOW_FRAMES     100 /* ~1.5 s */
/* At most this many of them, the most recent (tile 1 is short of RAM) */
#define MIC_FRONTEND_WAKE_HISTORY           80
/* The wake word direction is the mean of the largest group of DOA results within this angle ... */
#define MIC_FRONTEND_WAKE_CLUSTER_DEG       30
/* ... which needs at least this many of them (~75 ms of located speech) ... */
#define MIC_FRONTEND_WAKE_MIN_RESULTS       5
/* ... and at least this share of the confidence of all results in the window */
#define MIC_FRONTEND_WAKE_MIN_SHARE         0.25f
/*
 * Groups are ranked by confidence * (1 - NOVELTY * share of idle speech from
 * their direction over the last ~MIC_FRONTEND_IDLE_DOA_FRAMES located frames).
 * A TV that talks all day loses up to half its weight, so a wake word said
 * while it talks wins with about as many located frames as the TV had in the
 * window. A talker who always speaks from the same place still wins over a
 * stray voice with half their frames.
 */
#define MIC_FRONTEND_WAKE_NOVELTY           0.5f
#define MIC_FRONTEND_IDLE_DOA_FRAMES        2000 /* ~30 s of located speech */
#define MIC_FRONTEND_IDLE_DOA_BINS          36   /* 10 degrees each */
/*
 * No speech-driven re-steering while a conversation is active
 * (mic_frontend_set_steering()), for at most this long in case its end is
 * never reported.
 */
#define MIC_FRONTEND_CONVERSATION_MAX_FRAMES 4000 /* 60 s */

/* High-pass filter on the two pipeline channels, below the wake word and speech band */
#define MIC_HPF_CUTOFF_HZ                   100
#define MIC_HPF_SAMPLE_RATE                 16000
#define MIC_HPF_CHANNELS                    2

typedef struct {
    uint32_t frame;             /* mic_frontend_state_t.frame_count when it was produced */
    int16_t x_q14;              /* unit vector of the azimuth, Q14: cos, sin */
    int16_t y_q14;
    uint8_t confidence;
} mic_frontend_doa_entry_t;

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
    uint8_t steer_policy;        /* steer_policy_t */
    uint8_t conversation;        /* a conversation holds the steering (within its time limit) */
    uint32_t conversation_frames;
    uint32_t frame_count;        /* frames since init, stamps the DOA history */
    /* The last MIC_FRONTEND_WAKE_HISTORY DOA results (updated frames only), a ring */
    mic_frontend_doa_entry_t doa_history[MIC_FRONTEND_WAKE_HISTORY];
    uint32_t doa_history_next;
    uint32_t doa_history_len;
    float idle_doa[MIC_FRONTEND_IDLE_DOA_BINS];    /* where idle speech came from, decaying histogram */
    uint16_t wake_azimuth_deg;   /* direction of the last located wake word, MIC_FRONTEND_NO_BEAM if none */
    beamformer_t beam;
} mic_frontend_state_t;

/* Second order Butterworth high-pass per channel, coefficients Q28 */
typedef struct {
    int32_t b0;
    int32_t b1;
    int32_t b2;
    int32_t a1;
    int32_t a2;
    int32_t x1[MIC_HPF_CHANNELS];
    int32_t x2[MIC_HPF_CHANNELS];
    int32_t y1[MIC_HPF_CHANNELS];
    int32_t y2[MIC_HPF_CHANNELS];
} mic_hpf_t;

void mic_frontend_init(mic_frontend_state_t *state);

/**
 * Advances the AUTO_PAIR selection or the BEAM steering with this frame's DOA
 * result. Call once per frame before mic_frontend_process(); it has no effect
 * in the other modes.
 *
 * Entering BEAM points the beams at the DOA result straight away if it is
 * valid, without a crossfade, and does not wait out the dwell of an earlier
 * switch: the mode change makes the AEC and IC re-adapt anyway. Under
 * STEER_POLICY_WAKE_WORD it points at the last located wake word instead (the
 * DOA if there was none yet), under STEER_POLICY_FIXED it keeps the steering.
 *
 * Also records this frame's DOA result for mic_frontend_wake_word(), in every mode.
 */
void mic_frontend_update(mic_frontend_state_t *state,
                         mic_mode_t mode,
                         const doa_result_t *doa);

/**
 * Sets the steering policy and whether a conversation is active. Call once per
 * frame before mic_frontend_update(). While a conversation is active the talker
 * is known, so speech does not re-steer; after
 * MIC_FRONTEND_CONVERSATION_MAX_FRAMES the hold lapses until the next
 * conversation.
 */
void mic_frontend_set_steering(mic_frontend_state_t *state,
                               steer_policy_t policy,
                               int conversation_active);

/**
 * A wake word was just detected. Locates it from the DOA results of the last
 * MIC_FRONTEND_WAKE_WINDOW_FRAMES and, unless the policy is
 * STEER_POLICY_FIXED, steers BEAM there at once (crossfaded over the next
 * frame, without the stable time and the dwell) or switches AUTO_PAIR to the
 * mic facing it. Call after mic_frontend_update() for the frame.
 *
 * \return 1 if the wake word was located, 0 if there were too few or too
 *         scattered DOA results
 */
int mic_frontend_wake_word(mic_frontend_state_t *state, mic_mode_t mode);

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

void mic_hpf_init(mic_hpf_t *hpf);
/** Clears the filter history, e.g. before the filter is used again after a pause. */
void mic_hpf_reset(mic_hpf_t *hpf);
/**
 * Filters MIC_HPF_CHANNELS consecutive channels of frame_count samples in
 * place, saturating to +-INT32_MAX.
 */
void mic_hpf_process(mic_hpf_t *hpf, int32_t *samples, size_t frame_count);

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
