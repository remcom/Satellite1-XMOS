#pragma once

#include <stdint.h>

#include "audio/doa.h"
#include "audio/mic_frontend.h"

/*
 * Runtime audio settings and the latest DOA result, shared between the audio
 * pipeline, the audio servicer and the LED ring on the mic array tile.
 * Every value is a single aligned word, so readers need no lock.
 */

#define AUDIO_CTRL_MIC_GAIN_DB_MAX  30

/* Signal on the right (channel 1) output; the left one is always AEC+IC+NS+AGC */
typedef enum {
    CH1_TAP_NS  = 0,    /* AEC+IC+NS */
    CH1_TAP_IC  = 1,    /* AEC+IC, before the NS */
    CH1_TAP_AEC = 2,    /* AEC only: the target beam or mic after the echo canceller */
    CH1_TAP_COUNT
} ch1_tap_t;

/*
 * Settings tile 0 needs, sent along with every frame (frame_data_t.t0_ctrl)
 * since they are set on this tile.
 */
#define AUDIO_CTRL_T0_CH1_TAP(word)     ((ch1_tap_t)((word) & 0xFF))
#define AUDIO_CTRL_T0_IC_FAST_HOLD(word) (((word) >> 8) & 1)

/** Also sets the defaults of the settings below (see the audio servicer commands). */
void audio_ctrl_init(unsigned mic_gain_db, mic_mode_t mic_mode);

/** \return 0 on success, -1 if gain_db exceeds AUDIO_CTRL_MIC_GAIN_DB_MAX */
int audio_ctrl_set_mic_gain_db(unsigned gain_db);
unsigned audio_ctrl_get_mic_gain_db(void);
int32_t audio_ctrl_get_mic_gain_q16(void);

/** \return 0 on success, -1 for an unknown mode */
int audio_ctrl_set_mic_mode(unsigned mode);
mic_mode_t audio_ctrl_get_mic_mode(void);

/** \return 0 on success, -1 for an unknown tap */
int audio_ctrl_set_ch1_tap(unsigned tap);
/**
 * Limits audio_ctrl_set_ch1_tap() to the taps the pipeline of this build can
 * produce, a bit per ch1_tap_t; CH1_TAP_NS is always allowed. All by default.
 */
void audio_ctrl_set_ch1_taps(uint32_t taps);
ch1_tap_t audio_ctrl_get_ch1_tap(void);

/** Fast attack of the VNR that decides when the IC stops adapting (0 = fwk_voice EMA only). */
void audio_ctrl_set_ic_fast_hold(unsigned enable);
unsigned audio_ctrl_get_ic_fast_hold(void);

/** tile 0 settings for frame_data_t.t0_ctrl, see AUDIO_CTRL_T0_*() */
uint32_t audio_ctrl_get_t0_word(void);

/** High-pass filter (MIC_HPF_CUTOFF_HZ) on both pipeline channels before the AEC. */
void audio_ctrl_set_hpf(unsigned enable);
unsigned audio_ctrl_get_hpf(void);

/** \return 0 on success, -1 for an unknown policy */
int audio_ctrl_set_steer_policy(unsigned policy);
steer_policy_t audio_ctrl_get_steer_policy(void);

/**
 * active != 0: a wake word was detected just now and a conversation starts.
 * active == 0: the conversation ended.
 */
void audio_ctrl_wake_event(unsigned active);
/** Number of wake words reported since boot (wraps). */
uint32_t audio_ctrl_get_wake_count(void);
/** The last reported wake event. */
unsigned audio_ctrl_get_conversation(void);
/** 1 while the conversation is active and the front end still holds the steering for it. */
unsigned audio_ctrl_get_conversation_held(void);

/**
 * Called by the pipeline once per frame. lapsed: the conversation hold ran out
 * (mic_frontend_conversation_lapsed()).
 */
void audio_ctrl_publish(const doa_result_t *doa, uint8_t ch0_mask, uint8_t ch1_mask,
                        uint16_t beam_azimuth_deg, int lapsed);

/** Latest DOA result. `updated` is always 0: a frame-long flag is too short
 *  for a polling reader, use audio_ctrl_get_doa_update_count() instead. */
doa_result_t audio_ctrl_get_doa(void);
/** Number of new DOA results since boot (wraps). Read it before
 *  audio_ctrl_get_doa() so a changed count is never paired with an older result. */
uint32_t audio_ctrl_get_doa_update_count(void);
void audio_ctrl_get_mic_masks(uint8_t *ch0_mask, uint8_t *ch1_mask);
/** Beam steering in degrees, MIC_FRONTEND_NO_BEAM outside MIC_MODE_BEAM. */
uint16_t audio_ctrl_get_beam_azimuth(void);
