// Copyright 2022-2023 XMOS LIMITED.
// This Software is subject to the terms of the XMOS Public Licence: Version 1.

/* STD headers */
#include <string.h>
#include <stdint.h>
#include <xcore/hwtimer.h>

/* FreeRTOS headers */
#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include "queue.h"
#include "stream_buffer.h"

/* Library headers */
#include "generic_pipeline.h"

/* App headers */
#include "app_conf.h"
#include "speaker_pipeline.h"
#include "audio/frame_pool.h"

#if appconfAUDIO_PIPELINE_FRAME_ADVANCE * appconfAUDIO_SPK_PL_SR_FACTOR != appconfAUDIO_SPK_PIPELINE_FRAME_ADVANCE
#error The speaker pipeline frame advance needs to be set according the sample rate of both pipelines
#endif

/* Return values of a generic_pipeline output function */
#define SPEAKER_PIPELINE_DONT_FREE_FRAME 0

/* Frames in flight: one per stage plus the inter-stage queue (depth 2) */
#define SPEAKER_PIPELINE_FRAME_COUNT 4

typedef struct {
    int32_t samples[appconfAUDIO_PIPELINE_REF_CHANNELS][appconfAUDIO_SPK_PIPELINE_FRAME_ADVANCE];
} frame_data_t;

static frame_pool_t frame_pool;

static void *audio_pipeline_input_i(void *input_app_data)
{
    frame_data_t *frame_data = frame_pool_get(&frame_pool);

    speaker_pipeline_input(input_app_data,
                           &frame_data->samples[0][0],
                           appconfAUDIO_SPK_PIPELINE_FRAME_ADVANCE);

    return frame_data;
}

static int audio_pipeline_output_i(frame_data_t *frame_data,
                                   void *output_app_data)
{
    speaker_pipeline_output(output_app_data,
                            &frame_data->samples[0][0],
                            appconfAUDIO_SPK_PIPELINE_FRAME_ADVANCE);

    frame_pool_put(&frame_pool, frame_data);
    return SPEAKER_PIPELINE_DONT_FREE_FRAME;
}

void empty_stage(void)
{
    ;
}

static void initialize_pipeline_stages(void)
{
    frame_pool_init(&frame_pool, sizeof(frame_data_t), SPEAKER_PIPELINE_FRAME_COUNT);
}

void speaker_pipeline_init(
    void *input_app_data,
    void *output_app_data)
{
    const int stage_count = 2;

    const pipeline_stage_t stages[] = {
        (pipeline_stage_t)empty_stage,
        (pipeline_stage_t)empty_stage,
    };

    const configSTACK_DEPTH_TYPE stage_stack_sizes[] = {
        configMINIMAL_STACK_SIZE + RTOS_THREAD_STACK_SIZE(empty_stage) + RTOS_THREAD_STACK_SIZE(audio_pipeline_input_i),
        configMINIMAL_STACK_SIZE + RTOS_THREAD_STACK_SIZE(empty_stage) + RTOS_THREAD_STACK_SIZE(audio_pipeline_output_i),
    };

    initialize_pipeline_stages();

    generic_pipeline_init((pipeline_input_t)audio_pipeline_input_i,
                        (pipeline_output_t)audio_pipeline_output_i,
                        input_app_data,
                        output_app_data,
                        stages,
                        (const size_t*) stage_stack_sizes,
                        appconfAUDIO_PIPELINE_TASK_PRIORITY,
                        stage_count);
}
