// Copyright 2022-2023 XMOS LIMITED.
// This Software is subject to the terms of the XMOS Public Licence: Version 1.

#ifndef AUDIO_PIPELINE_DSP_H_
#define AUDIO_PIPELINE_DSP_H_

#include <stddef.h>
#include <stdint.h>
#include "FreeRTOS.h"
#include "stream_buffer.h"
#include "app_conf.h"
#include "audio/stage_stats.h"

/* Pipeline config */
#define AP_MAX_Y_CHANNELS (2)
#define AP_MAX_X_CHANNELS (2)
#define AP_FRAME_ADVANCE (240)

/* AEC config */
#define AEC_MAX_Y_CHANNELS   (AP_MAX_Y_CHANNELS)
#define AEC_MAX_X_CHANNELS   (AP_MAX_X_CHANNELS)
#define AEC_MAIN_FILTER_PHASES    (10)
#define AEC_SHADOW_FILTER_PHASES    (5)

/* Delay buffer config */
#define MAX_DELAY_BUF_CHANNELS (2)
#define DELAY_BUF_MAX_DELAY_MS                ( 150 )
#define DELAY_BUF_MAX_DELAY_SAMPLES           ( 16000*DELAY_BUF_MAX_DELAY_MS/1000 )

#include "aec_api.h"
#include "aec/aec_memory_pool.h"
#include "agc_api.h"
#include "ic_api.h"
#include "ns_api.h"
#include "vnr_features_api.h"
#include "vnr_inference_api.h"


/* Frames in flight per tile: one per stage plus the inter-stage queues (depth 2 each) */
#define AP_T1_FRAME_COUNT (2 + 2)
#define AP_T0_FRAME_COUNT (3 + 2 * 2)

/* Reference energy (mean square, full scale = 1.0) above which the speaker counts as playing */
#define AP_PLAYBACK_ENERGY_THRESHOLD (1e-7f)   /* ~-70 dBFS */

typedef struct {
    /* Channels being processed. After the AGC stage samples[0] is the final output. */
    int32_t samples[appconfAUDIO_PIPELINE_CHANNELS][appconfAUDIO_PIPELINE_FRAME_ADVANCE];

    /* Below is additional context needed by other stages on a per frame basis */
    int32_t vnr_pred_flag;
    float_s32_t max_ref_energy;
    float_s32_t aec_corr_factor;
    /* Tile 0 settings from audio_ctrl on tile 1, see AUDIO_CTRL_T0_*() */
    uint32_t t0_ctrl;
    /* Tile 1 stage stats, passed on to tile 0 where the stats servicer reads them */
    stage_stats_t1_report_t t1_stats;

    /*
     * The speaker reference until the AEC has run. Afterwards reused:
     *   [0] AEC+IC output (appconfAUDIO_PIPELINE_STORE_IC_AUDIO)
     *   [1] AEC+IC+NS output (appconfAUDIO_PIPELINE_STORE_NS_AUDIO), or the
     *       AEC output with CH1_TAP_AEC
     * Last in the frame, so tile 1 can leave it out when tile 0 does not read it.
     */
    int32_t DWORD_ALIGNED aec_reference_audio_samples[appconfAUDIO_PIPELINE_REF_CHANNELS][appconfAUDIO_PIPELINE_FRAME_ADVANCE];
} frame_data_t;

/*
 * Tile 0 outputs aec_reference_audio_samples[1] as its right channel. Only
 * when the NS stage does not overwrite it (the bypass build) is the reference
 * itself needed there; otherwise tile 1 sends the frame without it.
 */
#define AP_FRAME_SENDS_REF  (appconfAUDIO_PIPELINE_SKIP_NS || !appconfAUDIO_PIPELINE_STORE_NS_AUDIO)
/*
 * The ch1_tap_t outputs tile 0 can produce (audio_ctrl_set_ch1_taps()): the IC
 * and AEC taps are written by the IC stage, the IC one only while it is stored.
 */
#define AP_CH1_TAPS  ((1u << CH1_TAP_NS) | \
                      ((!appconfAUDIO_PIPELINE_SKIP_IC_AND_VNR && appconfAUDIO_PIPELINE_STORE_IC_AUDIO) ? (1u << CH1_TAP_IC) : 0) | \
                      (!appconfAUDIO_PIPELINE_SKIP_IC_AND_VNR ? (1u << CH1_TAP_AEC) : 0))
#define AP_FRAME_TX_BYTES   (AP_FRAME_SENDS_REF ? sizeof(frame_data_t) \
                                                : offsetof(frame_data_t, aec_reference_audio_samples))

typedef struct stage_delay_ctx {
    StreamBufferHandle_t delay_buf;
} stage_delay_ctx_t;

typedef struct aec_ctx {
    aec_state_t DWORD_ALIGNED aec_main_state;
    aec_state_t DWORD_ALIGNED aec_shadow_state;
    aec_shared_state_t DWORD_ALIGNED aec_shared_state;
    uint8_t DWORD_ALIGNED aec_main_memory_pool[sizeof(aec_memory_pool_t)];
    uint8_t DWORD_ALIGNED aec_shadow_memory_pool[sizeof(aec_shadow_filt_memory_pool_t)];
} aec_ctx_t;

typedef struct ic_stage_ctx {
    ic_state_t DWORD_ALIGNED state;
} ic_stage_ctx_t;

typedef struct vnr_pred_stage_ctx {
    vnr_pred_state_t vnr_pred_state;
} vnr_pred_stage_ctx_t;

typedef struct ns_stage_ctx {
    ns_state_t DWORD_ALIGNED state;
} ns_stage_ctx_t;

typedef struct agc_stage_ctx {
    agc_meta_data_t md;
    agc_state_t DWORD_ALIGNED state;
} agc_stage_ctx_t;

/**
 * Delay buffer contains the configured buffer delay size + 1 frame worth of audio
 * When appconfINPUT_SAMPLES_MIC_DELAY_MS  is positive, the mic is delayed
 * When appconfINPUT_SAMPLES_MIC_DELAY_MS is negative, the mic is "advanced" by delaying ref
 */
#define ABS(A) ((A >= 0) ? A : -A)
#define AP_INPUT_SAMPLES_MIC_DELAY_SIZE_PER_CHAN        ( 16000*ABS(appconfINPUT_SAMPLES_MIC_DELAY_MS)/1000 )
#define AP_INPUT_SAMPLES_MIC_DELAY_CHAN_CNT             ( (appconfINPUT_SAMPLES_MIC_DELAY_MS > 0) ? AEC_MAX_Y_CHANNELS : AEC_MAX_X_CHANNELS )
#define AP_INPUT_SAMPLES_MIC_DELAY_SIZE_CHAN            ( AP_INPUT_SAMPLES_MIC_DELAY_SIZE_PER_CHAN * AP_INPUT_SAMPLES_MIC_DELAY_CHAN_CNT )
#define AP_INPUT_SAMPLES_MIC_DELAY_SIZE_CUR_FRAME_WORDS ( AP_INPUT_SAMPLES_MIC_DELAY_CHAN_CNT * appconfAUDIO_PIPELINE_FRAME_ADVANCE)
#define AP_INPUT_SAMPLES_MIC_DELAY_CUR_FRAME_BYTES      ( AP_INPUT_SAMPLES_MIC_DELAY_SIZE_CUR_FRAME_WORDS * sizeof(int32_t))
#define AP_INPUT_SAMPLES_MIC_DELAY_BUF_SIZE_BYTES       ( AP_INPUT_SAMPLES_MIC_DELAY_SIZE_CHAN * sizeof(int32_t) )

void aec_process_frame_1thread(
        aec_state_t *main_state,
        aec_state_t *shadow_state,
        int32_t (*output_main)[AEC_FRAME_ADVANCE],
        int32_t (*output_shadow)[AEC_FRAME_ADVANCE],
        const int32_t (*y_data)[AEC_FRAME_ADVANCE],
        const int32_t (*x_data)[AEC_FRAME_ADVANCE]);

#endif /* AUDIO_PIPELINE_DSP_H_ */
