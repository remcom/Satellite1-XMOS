// Copyright 2022-2024 XMOS LIMITED.
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
#include "audio_pipeline.h"
#include "audio_pipeline_dsp.h"
#include "audio/audio_ctrl.h"
#include "audio/doa.h"
#include "audio/frame_pool.h"
#include "audio/mic_frontend.h"
#include "audio/stage_stats.h"

#if appconfAUDIO_PIPELINE_FRAME_ADVANCE != 240
#error This pipeline is only configured for 240 frame advance
#endif

#if appconfAUDIO_PIPELINE_MIC_COUNT != MIC_FRONTEND_MIC_COUNT
#error The mic front end expects MIC_FRONTEND_MIC_COUNT raw mics
#endif

#if appconfAUDIO_PIPELINE_FRAME_ADVANCE != DOA_FRAME_SIZE
#error DOA_FRAME_SIZE must match the pipeline frame advance
#endif

#if appconfAUDIO_PIPELINE_CHANNELS != MIC_HPF_CHANNELS
#error The high-pass filter expects appconfAUDIO_PIPELINE_CHANNELS channels
#endif

#if ON_TILE(1)
#if appconfINPUT_SAMPLES_MIC_DELAY_MS != 0
static stage_delay_ctx_t DWORD_ALIGNED delay_buf_state = {};
#endif
static aec_ctx_t DWORD_ALIGNED aec_state = {};

static frame_pool_t frame_pool;

/*
 * Raw mics of the frame currently in stage 0. Only the stage 0 task touches it:
 * the input function fills it and stage_frontend() consumes it before the next
 * input call.
 */
static int32_t DWORD_ALIGNED mic_raw[appconfAUDIO_PIPELINE_MIC_COUNT][appconfAUDIO_PIPELINE_FRAME_ADVANCE];
static mic_frontend_state_t mic_frontend_state;
static doa_state_t doa_state;
static mic_hpf_t mic_hpf;
static int mic_hpf_active;
/* audio_ctrl_get_wake_count() of the last wake word handed to the front end */
static uint32_t wake_count_seen;


static void *audio_pipeline_input_i(void *input_app_data)
{
    frame_data_t *frame_data = frame_pool_get(&frame_pool);
    /* The samples and the reference are overwritten below and by the front end */
    frame_data->vnr_pred_flag = 0;
    frame_data->aec_corr_factor = (float_s32_t){0, 0};
    frame_data->t0_ctrl = audio_ctrl_get_t0_word();

    audio_pipeline_input(input_app_data,
                         &frame_data->aec_reference_audio_samples[0][0],
                         &mic_raw[0][0],
                         appconfAUDIO_PIPELINE_FRAME_ADVANCE);

    return frame_data;
}

static int audio_pipeline_output_i(frame_data_t *frame_data,
                                   void *output_app_data)
{
    stage_stats_make_t1_report(&frame_data->t1_stats);

    rtos_intertile_tx(intertile_ctx,
                      appconfAUDIOPIPELINE_PORT,
                      frame_data,
                      AP_FRAME_TX_BYTES);

    frame_pool_put(&frame_pool, frame_data);
    return AUDIO_PIPELINE_DONT_FREE_FRAME;
}

/*
 * One pass over the reference gives both the playback gate for the DOA and
 * the AEC reference power the AGC reads (as aec_calc_max_input_energy()).
 */
static int playback_active(frame_data_t *frame_data)
{
    const float energy = audio_block_energies_s32(&frame_data->aec_reference_audio_samples[0][0],
                                                  appconfAUDIO_PIPELINE_REF_CHANNELS,
                                                  appconfAUDIO_PIPELINE_FRAME_ADVANCE,
                                                  &frame_data->max_ref_energy);
    return energy > AP_PLAYBACK_ENERGY_THRESHOLD;
}

/* Reduces the raw mics to the pipeline channels and updates the DOA estimate. */
static void stage_frontend(frame_data_t *frame_data)
{
    const mic_mode_t mode = audio_ctrl_get_mic_mode();

    const doa_result_t *doa = doa_process(&doa_state,
                                          &mic_raw[0][0],
                                          playback_active(frame_data));
    mic_frontend_set_steering(&mic_frontend_state,
                              audio_ctrl_get_steer_policy(),
                              audio_ctrl_get_conversation());
    mic_frontend_update(&mic_frontend_state, mode, doa);
    const uint32_t wake_count = audio_ctrl_get_wake_count();
    if (wake_count != wake_count_seen) {
        /* Before the wake sound plays: the DOA results of the wake word are in the history */
        wake_count_seen = wake_count;
        (void) mic_frontend_wake_word(&mic_frontend_state, mode);
    }
    mic_frontend_process(&mic_frontend_state,
                         mode,
                         &mic_raw[0][0],
                         &frame_data->samples[0][0],
                         appconfAUDIO_PIPELINE_FRAME_ADVANCE);

    /*
     * Before the AEC: it is linear, so the filter on the mics just becomes part
     * of the echo path it models.
     */
    if (audio_ctrl_get_hpf()) {
        if (!mic_hpf_active) {
            mic_hpf_reset(&mic_hpf);
            mic_hpf_active = 1;
        }
        mic_hpf_process(&mic_hpf, &frame_data->samples[0][0], appconfAUDIO_PIPELINE_FRAME_ADVANCE);
    } else {
        mic_hpf_active = 0;
    }

    uint8_t ch0_mask;
    uint8_t ch1_mask;
    mic_frontend_get_masks(&mic_frontend_state, mode, &ch0_mask, &ch1_mask);
    audio_ctrl_publish(doa, ch0_mask, ch1_mask,
                       mic_frontend_get_beam_azimuth(&mic_frontend_state, mode),
                       mic_frontend_conversation_lapsed(&mic_frontend_state));
}

static void stage_delay(frame_data_t *frame_data)
{
#if appconfAUDIO_PIPELINE_SKIP_STATIC_DELAY
#else
#if (appconfINPUT_SAMPLES_MIC_DELAY_MS > 0) /* Delay mics */
    size_t bytes_sent = xStreamBufferSend(
                                delay_buf_state.delay_buf,
                                &frame_data->samples,
                                AP_INPUT_SAMPLES_MIC_DELAY_CUR_FRAME_BYTES,
                                0);

    configASSERT(bytes_sent == AP_INPUT_SAMPLES_MIC_DELAY_CUR_FRAME_BYTES);

    if (xStreamBufferBytesAvailable(delay_buf_state.delay_buf) > AP_INPUT_SAMPLES_MIC_DELAY_BUF_SIZE_BYTES) {
        size_t bytes_rx = xStreamBufferReceive(
                                    delay_buf_state.delay_buf,
                                    &frame_data->samples,
                                    AP_INPUT_SAMPLES_MIC_DELAY_CUR_FRAME_BYTES,
                                    0);

        configASSERT(bytes_rx == AP_INPUT_SAMPLES_MIC_DELAY_CUR_FRAME_BYTES);
    }
#elif (appconfINPUT_SAMPLES_MIC_DELAY_MS < 0) /* Delay Ref*/
    size_t bytes_sent = xStreamBufferSend(
                                delay_buf_state.delay_buf,
                                &frame_data->aec_reference_audio_samples,
                                AP_INPUT_SAMPLES_MIC_DELAY_CUR_FRAME_BYTES,
                                0);

    configASSERT(bytes_sent == AP_INPUT_SAMPLES_MIC_DELAY_CUR_FRAME_BYTES);

    if (xStreamBufferBytesAvailable(delay_buf_state.delay_buf) > AP_INPUT_SAMPLES_MIC_DELAY_BUF_SIZE_BYTES) {
        size_t bytes_rx = xStreamBufferReceive(
                                    delay_buf_state.delay_buf,
                                    &frame_data->aec_reference_audio_samples,
                                    AP_INPUT_SAMPLES_MIC_DELAY_CUR_FRAME_BYTES,
                                    0);

        configASSERT(bytes_rx == AP_INPUT_SAMPLES_MIC_DELAY_CUR_FRAME_BYTES);
    }
#else /* Delay None */
#endif
#endif /* appconfAUDIO_PIPELINE_SKIP_DELAY */
}

static void stage_frontend_and_delay(frame_data_t *frame_data)
{
    const uint32_t start = stage_stats_start();
    stage_frontend(frame_data);
    stage_delay(frame_data);
    stage_stats_end(STAGE_STATS_T1_FRONTEND, start);
}

static void stage_aec(frame_data_t *frame_data)
{
    const uint32_t start = stage_stats_start();
#if !appconfAUDIO_PIPELINE_SKIP_AEC
    /*
     * In place: aec_frame_init() copies the mics into the AEC state before any
     * output is written. max_ref_energy was set by stage_frontend().
     */
    aec_process_frame_1thread(
            &aec_state.aec_main_state,
            &aec_state.aec_shadow_state,
            frame_data->samples,
            NULL,
            (const int32_t (*)[AEC_FRAME_ADVANCE])frame_data->samples,
            frame_data->aec_reference_audio_samples);

#if (appconfINPUT_SAMPLES_MIC_DELAY_MS < 0) && !appconfAUDIO_PIPELINE_SKIP_STATIC_DELAY
    /* stage_delay() delayed the reference after stage_frontend() measured it */
    frame_data->max_ref_energy = aec_calc_max_input_energy(
                                    frame_data->aec_reference_audio_samples,
                                    aec_state.aec_main_state.shared_state->num_x_channels);
#endif
    frame_data->aec_corr_factor = aec_calc_corr_factor(&aec_state.aec_main_state, 0);
#endif

    /*
     * Mic gain after the AEC: amplifying before it would clip loud echo, which
     * the linear AEC filter cannot cancel.
     */
    mic_apply_gain_q16(&frame_data->samples[0][0],
                       appconfAUDIO_PIPELINE_CHANNELS * appconfAUDIO_PIPELINE_FRAME_ADVANCE,
                       audio_ctrl_get_mic_gain_q16());
    stage_stats_end(STAGE_STATS_T1_AEC, start);
}

static void initialize_pipeline_stages(void)
{
    frame_pool_init(&frame_pool, sizeof(frame_data_t), AP_T1_FRAME_COUNT);
    mic_frontend_init(&mic_frontend_state);
    audio_ctrl_set_ch1_taps(AP_CH1_TAPS);
    doa_init(&doa_state);
    mic_hpf_init(&mic_hpf);

#if (appconfINPUT_SAMPLES_MIC_DELAY_MS != 0)
    configASSERT(AP_INPUT_SAMPLES_MIC_DELAY_BUF_SIZE_BYTES > 0);
    delay_buf_state.delay_buf = xStreamBufferCreate((size_t)AP_INPUT_SAMPLES_MIC_DELAY_BUF_SIZE_BYTES + AP_INPUT_SAMPLES_MIC_DELAY_CUR_FRAME_BYTES, 0);
    configASSERT(delay_buf_state.delay_buf);
#endif

    aec_init(&aec_state.aec_main_state,
             &aec_state.aec_shadow_state,
             &aec_state.aec_shared_state,
             &aec_state.aec_main_memory_pool[0],
             &aec_state.aec_shadow_memory_pool[0],
             AEC_MAX_Y_CHANNELS,
             AEC_MAX_X_CHANNELS,
             AEC_MAIN_FILTER_PHASES,
             AEC_SHADOW_FILTER_PHASES);
}

void audio_pipeline_init(
    void *input_app_data,
    void *output_app_data)
{
    const int stage_count = 2;
    const pipeline_stage_t stages[] = {
        (pipeline_stage_t)stage_frontend_and_delay,
        (pipeline_stage_t)stage_aec,
    };

    const configSTACK_DEPTH_TYPE stage_stack_sizes[] = {
        configMINIMAL_STACK_SIZE + RTOS_THREAD_STACK_SIZE(stage_frontend_and_delay) + RTOS_THREAD_STACK_SIZE(audio_pipeline_input_i),
        configMINIMAL_STACK_SIZE + RTOS_THREAD_STACK_SIZE(stage_aec) + RTOS_THREAD_STACK_SIZE(audio_pipeline_output_i),
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
#endif /* ON_TILE(1) */
