#pragma once

#include <stdint.h>

#include "agc.h"

/*
 * Level control of the wake word channel (channel 1 of the output).
 *
 * The wake word engine on the ESP32 (micro_wake_word, gain_factor 4) used to
 * get this channel at the fixed mic gain: speech ~5 dB louder than usual
 * clipped in its x4, and speech 10-15 dB softer reached it at -35..-40 dBFS
 * (simulated). A lib_voice AGC with a profile of its own levels it instead.
 *
 * The AGC works on the level the wake word engine sees: its targets and its
 * soft clipping (above 0.5) assume the ESP32's x4, and the output is divided
 * by WAKE_AGC_HOST_GAIN for it. So an ESP32 firmware that keeps gain_factor 4
 * gets speech peaks of about -9..-3 dBFS, soft clipped above -6 dBFS, and
 * never more than full scale.
 *
 * Unlike the AGC of channel 0 (AGC_PROFILE_ASR, up to +-1.6 dB a frame) it
 * raises the gain slowly, so it does not pump within a word, and lowers it
 * fast, so a loud word close to the device is brought down within ~150 ms
 * (soft clipping holds it under full scale until then). It adapts on speech only (the VNR of
 * the IC output) and on peaks above the upper threshold; in silence the gain
 * holds.
 */
#define WAKE_AGC_HOST_GAIN_SHIFT    2   /* the ESP32 multiplies by 4 */
#define WAKE_AGC_HOST_GAIN          (1 << WAKE_AGC_HOST_GAIN_SHIFT)

/*
 * +0.2 dB a frame (~13 dB/s), only while the frame peaks are below the lower
 * threshold. Simulated, against +0.1 dB: soft speech (10 dB under the usual
 * level, in noise) reaches the wake word engine 3 dB louder, nothing else
 * changed. Overridable to tune it in the simulator.
 */
#ifndef WAKE_AGC_GAIN_INC
#define WAKE_AGC_GAIN_INC   1.0233f
#endif

#define WAKE_AGC_PROFILE (agc_config_t){ \
    .adapt = 1, \
    .adapt_on_vnr = 1, \
    .vnr_threshold = f32_to_float_s32(0.5), \
    .soft_clipping = 1, \
    /* x4 at the start: the fixed gain the ESP32 applied before */ \
    .gain = f32_to_float_s32(4.0f), \
    .max_gain = f32_to_float_s32(32.0f),        /* +18 dB over that */ \
    .min_gain = f32_to_float_s32(0.4f),         /* -20 dB */ \
    /* frame peaks (smoothed) between -12 and -6 dBFS at the wake word engine */ \
    .upper_threshold = f32_to_float_s32(0.5f), \
    .lower_threshold = f32_to_float_s32(0.25f), \
    .gain_inc = f32_to_float_s32(WAKE_AGC_GAIN_INC),  \
    .gain_dec = f32_to_float_s32(0.944f),       /* -0.5 dB a frame, ~33 dB/s */ \
    .lc_enabled = 0, \
    .lc_n_frame_far = 0, \
    .lc_n_frame_near = 0, \
    .lc_corr_threshold = f32_to_float_s32(0), \
    .lc_bg_power_gamma = f32_to_float_s32(0), \
    .lc_gamma_inc = f32_to_float_s32(0), \
    .lc_gamma_dec = f32_to_float_s32(0), \
    .lc_far_delta = f32_to_float_s32(0), \
    .lc_near_delta = f32_to_float_s32(0), \
    .lc_near_delta_far_active = f32_to_float_s32(0), \
    .lc_gain_max = f32_to_float_s32(0), \
    .lc_gain_double_talk = f32_to_float_s32(0), \
    .lc_gain_silence = f32_to_float_s32(0), \
    .lc_gain_min = f32_to_float_s32(0), \
    .lc_vnr_low = f32_to_float_s32(0.2), \
    .lc_vnr_low_count_limit = 17, \
    .startup_delay = 0, \
    }

typedef struct {
    agc_state_t agc;
    agc_meta_data_t md;
} wake_agc_t;

void wake_agc_init(wake_agc_t *state);

/**
 * Levels one frame (AGC_FRAME_ADVANCE samples) of the wake word channel in
 * place. vnr: speech probability of the frame (the IC output VNR).
 */
void wake_agc_process(wake_agc_t *state, int32_t *samples, float_s32_t vnr);

/** Current gain in dB, as the wake word engine sees it (including the host's x4). */
float wake_agc_gain_db(const wake_agc_t *state);
