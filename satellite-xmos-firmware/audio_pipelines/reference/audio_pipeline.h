// Copyright 2022-2023 XMOS LIMITED.
// This Software is subject to the terms of the XMOS Public Licence: Version 1.

#ifndef AUDIO_PIPELINE_H_
#define AUDIO_PIPELINE_H_

#include <stddef.h>
#include <stdint.h>
#include "app_conf.h"

/* Return values of a generic_pipeline output function */
#define AUDIO_PIPELINE_DONT_FREE_FRAME 0
#define AUDIO_PIPELINE_FREE_FRAME      1

void audio_pipeline_init(
        void *input_app_data,
        void *output_app_data);

/**
 * Receives one block of audio. Called on the mic array tile.
 *
 * \param ref         appconfAUDIO_PIPELINE_REF_CHANNELS x frame_count samples,
 *                    channel major: the speaker reference
 * \param mics        appconfAUDIO_PIPELINE_MIC_COUNT x frame_count samples,
 *                    channel major, in MIC_ARRAY_CONFIG_INPUT_MAPPING order
 * \param frame_count must be appconfAUDIO_PIPELINE_FRAME_ADVANCE
 */
void audio_pipeline_input(
        void *input_app_data,
        int32_t *ref,
        int32_t *mics,
        size_t frame_count);

/**
 * Sends one block of processed audio to the host over I2S.
 *
 * \param left        frame_count samples for the left I2S channel
 * \param right       frame_count samples for the right I2S channel
 * \param frame_count must be appconfAUDIO_PIPELINE_FRAME_ADVANCE
 */
void audio_pipeline_output(
        void *output_app_data,
        const int32_t *left,
        const int32_t *right,
        size_t frame_count);

#endif /* AUDIO_PIPELINE_H_ */
