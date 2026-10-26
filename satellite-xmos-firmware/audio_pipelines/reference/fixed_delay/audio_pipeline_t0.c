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
#include "aec_api.h"
#include "agc_api.h"
#include "ic_api.h"
#include "ns_api.h"
#include "vnr_features_api.h"
#include "vnr_inference_api.h"

/* App headers */
#include "app_conf.h"
#include "audio_pipeline.h"
#include "audio_pipeline_dsp.h"
#include "audio/audio_ctrl.h"
#include "audio/frame_pool.h"
#include "audio/stage_stats.h"
#include "audio/vnr_hold.h"

#if appconfAUDIO_PIPELINE_FRAME_ADVANCE != 240
#error This pipeline is only configured for 240 frame advance
#endif

#define VNR_AGC_THRESHOLD (0.5)

#if ON_TILE(0)
static ic_stage_ctx_t DWORD_ALIGNED ic_stage_state = {};
static vnr_pred_stage_ctx_t DWORD_ALIGNED vnr_pred_stage_state = {};
static ns_stage_ctx_t DWORD_ALIGNED ns_stage_state = {};
static agc_stage_ctx_t DWORD_ALIGNED agc_stage_state = {};
static vnr_hold_t vnr_hold;

static frame_pool_t frame_pool;

static void *audio_pipeline_input_i(void *input_app_data)
{
    frame_data_t *frame_data = frame_pool_get(&frame_pool);

    size_t bytes_received = 0;
    bytes_received = rtos_intertile_rx_len(
            intertile_ctx,
            appconfAUDIOPIPELINE_PORT,
            portMAX_DELAY);

    xassert(bytes_received == AP_FRAME_TX_BYTES);

    rtos_intertile_rx_data(
            intertile_ctx,
            frame_data,
            bytes_received);

    stage_stats_merge_t1_report(&frame_data->t1_stats);

    return frame_data;
}

static int audio_pipeline_output_i(frame_data_t *frame_data,
                                   void *output_app_data)
{
    /*
     * left:  AEC+IC+NS+AGC
     * right: without AGC, by AUDIO_CTRL_T0_CH1_TAP(): AEC+IC+NS (default),
     *        AEC+IC or AEC only. When the IC or NS stage is skipped (bypass
     *        build) this buffer still holds reference channel 1.
     */
    const int32_t *right = frame_data->aec_reference_audio_samples[1];
#if !appconfAUDIO_PIPELINE_SKIP_IC_AND_VNR && appconfAUDIO_PIPELINE_STORE_IC_AUDIO
    if (AUDIO_CTRL_T0_CH1_TAP(frame_data->t0_ctrl) == CH1_TAP_IC) {
        right = frame_data->aec_reference_audio_samples[0];
    }
#endif
    audio_pipeline_output(output_app_data,
                          frame_data->samples[0],
                          right,
                          appconfAUDIO_PIPELINE_FRAME_ADVANCE);

    frame_pool_put(&frame_pool, frame_data);
    return AUDIO_PIPELINE_DONT_FREE_FRAME;
}

/*
 * ic_calc_vnr_pred(), which also returns this frame's unsmoothed VNR of the IC
 * input for the fast attack hold. Same inference calls, so no extra cost.
 */
static float_s32_t calc_vnr_pred(ic_state_t *ic, vnr_pred_state_t *pred)
{
    bfp_s32_t feature_patch;
    int32_t feature_patch_data[VNR_PATCH_WIDTH * VNR_MEL_FILTERS];
    vnr_pred_state_t *ic_pred = &ic->vnr_pred_state;

    float_s32_t input_vnr;
    vnr_extract_features(&ic_pred->feature_state[0], &feature_patch, feature_patch_data, &ic->Y_bfp[0]);
    vnr_inference(&input_vnr, &feature_patch);
    ic_pred->input_vnr_pred = float_s32_ema(ic_pred->input_vnr_pred, input_vnr, ic_pred->pred_alpha_q30);

    float_s32_t output_vnr;
    vnr_extract_features(&ic_pred->feature_state[1], &feature_patch, feature_patch_data, &ic->Error_bfp[0]);
    vnr_inference(&output_vnr, &feature_patch);
    ic_pred->output_vnr_pred = float_s32_ema(ic_pred->output_vnr_pred, output_vnr, ic_pred->pred_alpha_q30);

    pred->input_vnr_pred = ic_pred->input_vnr_pred;
    pred->output_vnr_pred = ic_pred->output_vnr_pred;
    return input_vnr;
}

static void stage_vnr_and_ic(frame_data_t *frame_data)
{
    const uint32_t start = stage_stats_start();
#if appconfAUDIO_PIPELINE_SKIP_IC_AND_VNR
#else
    if (AUDIO_CTRL_T0_CH1_TAP(frame_data->t0_ctrl) == CH1_TAP_AEC) {
        /* Free on this tile: tile 1 sends no reference unless the NS output is not stored there */
        memcpy(frame_data->aec_reference_audio_samples[1], frame_data->samples[0], appconfAUDIO_PIPELINE_FRAME_ADVANCE * sizeof(int32_t));
    }

    /*
     * In place: ic_filter() is done with its y input once it has copied it into
     * the IC state, before it writes the output (checked bit exact on xcore).
     */
    ic_filter(&ic_stage_state.state,
              frame_data->samples[0],
              frame_data->samples[1],
              frame_data->samples[0]);

    vnr_pred_state_t *vnr_pred_state = &vnr_pred_stage_state.vnr_pred_state;
    const float_s32_t input_vnr = calc_vnr_pred(&ic_stage_state.state, vnr_pred_state);

    float_s32_t agc_vnr_threshold = f32_to_float_s32(VNR_AGC_THRESHOLD);
    frame_data->vnr_pred_flag = float_s32_gt(vnr_pred_stage_state.vnr_pred_state.output_vnr_pred, agc_vnr_threshold);

    /* Tracked either way, so switching the fast hold on starts from the current estimate */
    const float hold_vnr = vnr_hold_update(&vnr_hold, float_s32_to_float(input_vnr));
    ic_adapt(&ic_stage_state.state,
             AUDIO_CTRL_T0_IC_FAST_HOLD(frame_data->t0_ctrl) ? f32_to_float_s32(hold_vnr)
                                                              : vnr_pred_state->input_vnr_pred);

    /* Intentionally ignoring comms ch from here on out */
#if appconfAUDIO_PIPELINE_STORE_IC_AUDIO    
    memcpy(frame_data->aec_reference_audio_samples[0], frame_data->samples[0], appconfAUDIO_PIPELINE_FRAME_ADVANCE * sizeof(int32_t));   // Store the interference cancelled audio in the first reference channel
#endif
#endif
    stage_stats_end(STAGE_STATS_T0_IC_VNR, start);
}

static void stage_ns(frame_data_t *frame_data)
{
    const uint32_t start = stage_stats_start();
#if appconfAUDIO_PIPELINE_SKIP_NS
#else
    configASSERT(NS_FRAME_ADVANCE == appconfAUDIO_PIPELINE_FRAME_ADVANCE);
    /* ns_api.h: input and output may be the same buffer */
    ns_process_frame(
                &ns_stage_state.state,
                frame_data->samples[0],
                frame_data->samples[0]);
#if appconfAUDIO_PIPELINE_STORE_NS_AUDIO
    if (AUDIO_CTRL_T0_CH1_TAP(frame_data->t0_ctrl) != CH1_TAP_AEC) {
        memcpy(frame_data->aec_reference_audio_samples[1], frame_data->samples[0], appconfAUDIO_PIPELINE_FRAME_ADVANCE * sizeof(int32_t));   // Store NS audio in the second reference channel
    }
#endif
#endif
    stage_stats_end(STAGE_STATS_T0_NS, start);
}

static void stage_agc(frame_data_t *frame_data)
{
    const uint32_t start = stage_stats_start();
#if appconfAUDIO_PIPELINE_SKIP_AGC
#else
    configASSERT(AGC_FRAME_ADVANCE == appconfAUDIO_PIPELINE_FRAME_ADVANCE);

    agc_stage_state.md.vnr_flag = frame_data->vnr_pred_flag;
    agc_stage_state.md.aec_ref_power = frame_data->max_ref_energy;
    agc_stage_state.md.aec_corr_factor = frame_data->aec_corr_factor;

    /* agc_api.h: input and output may be the same buffer */
    agc_process_frame(
            &agc_stage_state.state,
            frame_data->samples[0],
            frame_data->samples[0],
            &agc_stage_state.md);
#endif
    stage_stats_end(STAGE_STATS_T0_AGC, start);
}

static void initialize_pipeline_stages(void)
{
    frame_pool_init(&frame_pool, sizeof(frame_data_t), AP_T0_FRAME_COUNT);

    ic_init(&ic_stage_state.state);
    vnr_hold_init(&vnr_hold, float_s32_to_float(ic_stage_state.state.vnr_pred_state.input_vnr_pred));

    ns_init(&ns_stage_state.state);

    agc_init(&agc_stage_state.state, &AGC_PROFILE_ASR);
    agc_stage_state.md.aec_ref_power = AGC_META_DATA_NO_AEC;
    agc_stage_state.md.aec_corr_factor = AGC_META_DATA_NO_AEC;
}

void audio_pipeline_init(
    void *input_app_data,
    void *output_app_data)
{
    const int stage_count = 3;
    const pipeline_stage_t stages[] = {
        (pipeline_stage_t)stage_vnr_and_ic,
        (pipeline_stage_t)stage_ns,
        (pipeline_stage_t)stage_agc,
    };

    const configSTACK_DEPTH_TYPE stage_stack_sizes[] = {
        configMINIMAL_STACK_SIZE + RTOS_THREAD_STACK_SIZE(stage_vnr_and_ic) + RTOS_THREAD_STACK_SIZE(audio_pipeline_input_i),
        configMINIMAL_STACK_SIZE + RTOS_THREAD_STACK_SIZE(stage_ns),
        configMINIMAL_STACK_SIZE + RTOS_THREAD_STACK_SIZE(stage_agc) + RTOS_THREAD_STACK_SIZE(audio_pipeline_output_i),
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

#endif /* ON_TILE(0)*/
