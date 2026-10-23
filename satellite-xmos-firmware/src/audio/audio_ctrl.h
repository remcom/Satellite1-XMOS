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

void audio_ctrl_init(unsigned mic_gain_db, mic_mode_t mic_mode);

/** \return 0 on success, -1 if gain_db exceeds AUDIO_CTRL_MIC_GAIN_DB_MAX */
int audio_ctrl_set_mic_gain_db(unsigned gain_db);
unsigned audio_ctrl_get_mic_gain_db(void);
int32_t audio_ctrl_get_mic_gain_q16(void);

/** \return 0 on success, -1 for an unknown mode */
int audio_ctrl_set_mic_mode(unsigned mode);
mic_mode_t audio_ctrl_get_mic_mode(void);

/** Called by the pipeline once per frame. */
void audio_ctrl_publish(const doa_result_t *doa, uint8_t ch0_mask, uint8_t ch1_mask);

doa_result_t audio_ctrl_get_doa(void);
void audio_ctrl_get_mic_masks(uint8_t *ch0_mask, uint8_t *ch1_mask);
