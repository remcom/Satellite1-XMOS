// Copyright 2022-2023 XMOS LIMITED.
// This Software is subject to the terms of the XMOS Public Licence: Version 1.

#ifndef SPEAKER_PIPELINE_H_
#define SPEAKER_PIPELINE_H_

#include <stddef.h>
#include <stdint.h>
#include "app_conf.h"

void speaker_pipeline_init(
        void *input_app_data,
        void *output_app_data);

/**
 * Receives one block of speaker audio from the host.
 *
 * \param samples     appconfAUDIO_PIPELINE_REF_CHANNELS x frame_count samples, channel major
 * \param frame_count must be appconfAUDIO_SPK_PIPELINE_FRAME_ADVANCE
 */
void speaker_pipeline_input(
        void *input_app_data,
        int32_t *samples,
        size_t frame_count);

/**
 * Sends one block to the DAC and passes it on as the AEC reference.
 *
 * \param samples     appconfAUDIO_PIPELINE_REF_CHANNELS x frame_count samples, channel major
 * \param frame_count must be appconfAUDIO_SPK_PIPELINE_FRAME_ADVANCE
 */
void speaker_pipeline_output(
        void *output_app_data,
        const int32_t *samples,
        size_t frame_count);

#endif /* SPEAKER_PIPELINE_H_ */
