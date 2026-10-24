// Copyright 2022-2023 XMOS LIMITED.
// This Software is subject to the terms of the XMOS Public Licence: Version 1.

/* STD headers */
#include <stdint.h>

/* FreeRTOS headers */
#include "FreeRTOS.h"
#include "task.h"

/* App headers */
#include "app_conf.h"
#include "speaker_pipeline.h"

#if appconfAUDIO_PIPELINE_FRAME_ADVANCE * appconfAUDIO_SPK_PL_SR_FACTOR != appconfAUDIO_SPK_PIPELINE_FRAME_ADVANCE
#error The speaker pipeline frame advance needs to be set according the sample rate of both pipelines
#endif

typedef struct {
    void *input_app_data;
    void *output_app_data;
} speaker_pipeline_ctx_t;

/*
 * The speaker path does no processing, so one task moves each block straight
 * from input to output. Running it as a multi-stage pipeline would only add a
 * frame of playback latency per stage and a task on the busy mic array tile.
 */
static int32_t samples[appconfAUDIO_PIPELINE_REF_CHANNELS][appconfAUDIO_SPK_PIPELINE_FRAME_ADVANCE];

static void speaker_pipeline_task(void *arg)
{
    const speaker_pipeline_ctx_t *ctx = arg;

    for (;;) {
        speaker_pipeline_input(ctx->input_app_data,
                               &samples[0][0],
                               appconfAUDIO_SPK_PIPELINE_FRAME_ADVANCE);
        speaker_pipeline_output(ctx->output_app_data,
                                &samples[0][0],
                                appconfAUDIO_SPK_PIPELINE_FRAME_ADVANCE);
    }
}

void speaker_pipeline_init(
    void *input_app_data,
    void *output_app_data)
{
    static speaker_pipeline_ctx_t ctx;
    ctx.input_app_data = input_app_data;
    ctx.output_app_data = output_app_data;

    xTaskCreate(speaker_pipeline_task,
                "speaker pipeline",
                configMINIMAL_STACK_SIZE + RTOS_THREAD_STACK_SIZE(speaker_pipeline_task),
                &ctx,
                appconfAUDIO_PIPELINE_TASK_PRIORITY,
                NULL);
}
