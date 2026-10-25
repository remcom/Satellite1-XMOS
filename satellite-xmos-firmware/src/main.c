// Copyright 2020-2024 XMOS LIMITED.
// This Software is subject to the terms of the XMOS Public Licence: Version 1.

#include <platform.h>
#include <xs1.h>
#include <xcore/channel.h>
#include <stdint.h>
#include <string.h>

/* FreeRTOS headers */
#include "FreeRTOS.h"
#include "task.h"
#include "stream_buffer.h"
#include "queue.h"

/* Library headers */
#include "rtos_printf.h"
#include "src.h"

/* App headers */
#include "app_conf.h"
#include "platform/platform_init.h"
#include "platform/driver_instances.h"
#include "platform/platform_conf.h"
#include "audio_pipeline.h"
#include "speaker_pipeline.h"
#include "dfu_servicer.h"
#include "gpio/gpio_servicer.h"
#include "led_ring/led_ring_servicer.h"
#include "audio_servicer/audio_servicer.h"
#include "stats_servicer/stats_servicer.h"
#include "audio/audio_ctrl.h"
#include "audio/stage_stats.h"
#include "audio/voice_src.h"


/* Config headers for sw_pll */
#include "sw_pll.h"

#define DEVICE_STATUS_READY_REGISTER_IDX   0
#define DEVICE_STATUS_READY_VALUE          1

/* One block of speaker reference at the pipeline rate, sample interleaved */
typedef struct {
    int32_t samples[appconfAUDIO_PIPELINE_FRAME_ADVANCE][appconfAUDIO_PIPELINE_REF_CHANNELS];
} ref_frame_t;

#if ON_TILE(SPEAKER_PIPELINE_TILE_NO)
/* Holds ref_frame_t by value: speaker pipeline -> audio pipeline */
rtos_osal_queue_t *ref_input_queue;
#endif

/* Frames the audio pipeline has output; the watchdog is only fed while it advances */
static volatile uint32_t audio_frames_out;

void speaker_pipeline_input(void *input_app_data,
                        int32_t *samples,
                        size_t frame_count)
{
#if ON_TILE(SPEAKER_PIPELINE_TILE_NO)
    /* This shouldn't need to block given it shares a clock with the PDM mics */

        xassert(frame_count == appconfAUDIO_SPK_PIPELINE_FRAME_ADVANCE);
        /* I2S provides sample channel format */
        int32_t tmp[appconfAUDIO_SPK_PIPELINE_FRAME_ADVANCE][appconfI2S_AUDIO_INPUTS][appconfAUDIO_PIPELINE_REF_CHANNELS];

        size_t rx_count =
        rtos_i2s_rx(i2s_ctx,
                    (int32_t*) tmp,
                    frame_count,
                    portMAX_DELAY);
        xassert(rx_count == frame_count);

        for (int i=0; i<frame_count; i++) {
            samples[i] = tmp[i][0][0];
            samples[i + frame_count] = tmp[i][0][1];
        }
#endif
}

void speaker_pipeline_output(void *output_app_data,
                        const int32_t *samples,
                        size_t frame_count)
{
#if ON_TILE(SPEAKER_PIPELINE_TILE_NO)    
    (void) output_app_data;

    xassert(frame_count == appconfAUDIO_SPK_PIPELINE_FRAME_ADVANCE);
    
    /* I2S expects sample channel format */
    int32_t tmp[appconfAUDIO_SPK_PIPELINE_FRAME_ADVANCE][1][appconfAUDIO_PIPELINE_REF_CHANNELS];
    for (int j=0; j<frame_count; j++) {
        tmp[j][0][0] = samples[j];                  // ref 0 -> DAC
        tmp[j][0][1] = samples[j + frame_count];    // ref 1 -> DAC
    }
    
    // send to DAC
    rtos_i2s_tx_1(i2s_ctx,
                (int32_t*) tmp,
                frame_count,
                portMAX_DELAY);
    
    ref_frame_t ref;
    const uint32_t src_start = stage_stats_start();

    // down sample reference signal to 16kHz if needed 
    if (appconfI2S_AUDIO_SAMPLE_RATE == 3*appconfAUDIO_PIPELINE_SAMPLE_RATE) {
        static voice_src_ds3_t src_state[2];

        for( int frame=0; frame < frame_count; frame +=3 ){
            for (int ch = 0; ch < appconfAUDIO_PIPELINE_REF_CHANNELS; ch++) {
                const int32_t in[3] = { tmp[frame][0][ch], tmp[frame+1][0][ch], tmp[frame+2][0][ch] };
                ref.samples[frame/3][ch] = voice_src_ds3(&src_state[ch], in);
            }
        }
    } else {
      memcpy( ref.samples, tmp, sizeof(ref.samples) );
    }
    stage_stats_end(STAGE_STATS_T1_SPK_SRC, src_start);

    /*
     * Send to the microphone pipeline as reference. Playback must not wait for
     * the mic pipeline: if the AEC falls behind, the oldest reference is
     * dropped instead of stalling the DAC.
     */
    if (rtos_osal_queue_send(ref_input_queue, &ref, 0) != RTOS_OSAL_SUCCESS) {
        static ref_frame_t stale;
        (void) rtos_osal_queue_receive(ref_input_queue, &stale, 0);
        (void) rtos_osal_queue_send(ref_input_queue, &ref, 0);
    }

#endif
}



void audio_pipeline_input(void *input_app_data,
                        int32_t *ref,
                        int32_t *mics,
                        size_t frame_count)
{
    (void) input_app_data;
    xassert(frame_count == appconfAUDIO_PIPELINE_FRAME_ADVANCE);

    /* The driver takes int32_t ** but fills a flat, channel major buffer */
    int32_t **mic_buf = (int32_t **) mics;

    static int flushed;
    while (!flushed) {
        size_t received;
        received = rtos_mic_array_rx(mic_array_ctx,
                                     mic_buf,
                                     frame_count,
                                     0);
        if (received == 0) {
            rtos_mic_array_rx(mic_array_ctx,
                              mic_buf,
                              frame_count,
                              portMAX_DELAY);
            flushed = 1;
        }
    }

#if ON_TILE(SPEAKER_PIPELINE_TILE_NO)
    //read the speaker pipeline output as reference
    ref_frame_t ref_frame;
    (void) rtos_osal_queue_receive(ref_input_queue, &ref_frame, RTOS_OSAL_WAIT_FOREVER);

    for (int i=0; i<frame_count; i++) {
        ref[i] = ref_frame.samples[i][0];
        ref[i + frame_count] = ref_frame.samples[i][1];
    }
#endif


    /* Receive the next frame from the PDM microphones. */
    rtos_mic_array_rx(mic_array_ctx,
                      mic_buf,
                      frame_count,
                      portMAX_DELAY);
}

void audio_pipeline_output(void *output_app_data,
                        const int32_t *left,
                        const int32_t *right,
                        size_t frame_count)
{
    (void) output_app_data;


#if appconfI2S_ENABLED

    xassert(frame_count == appconfAUDIO_PIPELINE_FRAME_ADVANCE);
    /* I2S expects sample channel format */
    int32_t tmp[appconfAUDIO_SPK_PIPELINE_FRAME_ADVANCE][1][2];

    const uint32_t src_start = stage_stats_start();
    if (appconfI2S_AUDIO_SAMPLE_RATE == 3*appconfAUDIO_PIPELINE_SAMPLE_RATE) {
        /* Up sample to 48kHz. A plain sample repeat would leave images above 8 kHz and droop the top of the band. */
        static voice_src_us3_t src_state[2];

        for( int in_frame=0, out_frame=0; in_frame < frame_count; in_frame++, out_frame += 3 ){
            int32_t out[2][3];
            voice_src_us3(&src_state[0], left[in_frame], out[0]);
            voice_src_us3(&src_state[1], right[in_frame], out[1]);
            for (int k = 0; k < 3; k++) {
                tmp[out_frame + k][0][0] = out[0][k];
                tmp[out_frame + k][0][1] = out[1][k];
            }
        }
    } else {
        for (int j=0; j<frame_count; j++) {
            tmp[j][0][0] = left[j];
            tmp[j][0][1] = right[j];
        }
    }
    stage_stats_end(STAGE_STATS_T0_OUT_SRC, src_start);

#if ON_TILE(I2S_TILE_NO)
    rtos_i2s_tx(i2s_ctx,
                (int32_t*) tmp,
                appconfAUDIO_SPK_PIPELINE_FRAME_ADVANCE,
                portMAX_DELAY);
#else
    /* To audio_output_task() on the I2S tile */
    rtos_intertile_tx(intertile_ctx,
                      appconfAUDIO_OUTPUT_PORT,
                      tmp,
                      sizeof(tmp));
#endif
#endif
    audio_frames_out = audio_frames_out + 1;
}

#if appconfI2S_ENABLED && ON_TILE(I2S_TILE_NO) && (I2S_TILE_NO != appconfAUDIO_PIPELINE_OUTPUT_TILE_NO)
/*
 * Plays the pipeline output that audio_pipeline_output() sends from the other
 * tile. Replaces an I2S RPC call, which allocated the ~5.8 KB frame on the heap
 * of both tiles for every frame, and a response message. Each frame is read
 * out of the intertile channel before it is written to I2S, so other intertile
 * traffic only waits behind it while the transfer itself runs. Runs at the
 * priority the I2S RPC host had, so it drains the channel just as promptly.
 */
static void audio_output_task(void *arg)
{
    (void) arg;
    static int32_t frame[appconfAUDIO_SPK_PIPELINE_FRAME_ADVANCE][2];

    for (;;) {
        const size_t len = rtos_intertile_rx_len(intertile_ctx,
                                                 appconfAUDIO_OUTPUT_PORT,
                                                 RTOS_OSAL_WAIT_FOREVER);
        xassert(len == sizeof(frame));
        rtos_intertile_rx_data(intertile_ctx, frame, len);

        rtos_i2s_tx(i2s_ctx,
                    &frame[0][0],
                    appconfAUDIO_SPK_PIPELINE_FRAME_ADVANCE,
                    portMAX_DELAY);
    }
}
#endif




void vApplicationMallocFailedHook(void)
{
    rtos_printf("Malloc Failed on tile %d!\n", THIS_XCORE_TILE);
    xassert(0);
    for(;;);
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void) xTask;
    rtos_printf("Stack overflow in task %s on tile %d!\n", pcTaskName, THIS_XCORE_TILE);
    xassert(0);
    for(;;);
}

static void init_watchdog(void)
{
    //xin : 24 Mhz, decrement WATCHDOG_COUNT every 2.7 ms:
    write_sswitch_reg_no_ack(get_local_tile_id(), XS1_SSWITCH_WATCHDOG_PRESCALER_WRAP_NUM, (0xFFFF));
    //trigger watchdog after ~11s of inactivity    
    write_sswitch_reg_no_ack(get_local_tile_id(), XS1_SSWITCH_WATCHDOG_COUNT_NUM, 0xFFF );
    write_sswitch_reg_no_ack(get_local_tile_id(), XS1_SSWITCH_WATCHDOG_CFG_NUM, (1 << XS1_WATCHDOG_COUNT_ENABLE_SHIFT) | (1 << XS1_WATCHDOG_TRIGGER_ENABLE_SHIFT) );
}

#if ON_TILE(0)
static void reset_watchdog(void)
{
    //reset watchdog to max
    write_sswitch_reg_no_ack(get_local_tile_id(), XS1_SSWITCH_WATCHDOG_COUNT_NUM, 0xFFF );
}
#endif

/* Heap usage is read with the stats servicer (STATS_SERVICER_CMD_HEAP_FREE) */
static void mem_analysis(void)
{
#if ON_TILE(0)
    uint32_t frames_at_last_check = audio_frames_out;
#endif
	for (;;) {
#if ON_TILE(0)
        /*
         * Tile 0 outputs every frame that made it through both tiles, so a
         * stalled pipeline on either tile stops the watchdog being fed.
         */
        const uint32_t frames = audio_frames_out;
        if (frames != frames_at_last_check) {
            reset_watchdog();
        }
        frames_at_last_check = frames;
#endif
        vTaskDelay(pdMS_TO_TICKS(5000));
	}
}

#if ON_TILE(0) && appconfDEVICE_CTRL_SPI
static void device_control_ready_task(void *arg)
{
    device_control_t *device_control_ctx = arg;

    xassert(device_control_ctx != NULL);

    while (device_control_ctx->status_buffer == NULL) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    device_control_set_resource_status(device_control_ctx,
                                       DEVICE_STATUS_READY_REGISTER_IDX,
                                       DEVICE_STATUS_READY_VALUE);

    vTaskDelete(NULL);
}
#endif

void startup_task(void *arg)
{
    rtos_printf("Startup task running from tile %d on core %d\n", THIS_XCORE_TILE, portGET_CORE_ID());
    stage_stats_init();
    voice_src_init();
    platform_start();

#if ON_TILE(MICARRAY_TILE_NO)
    /* Before the audio servicer and pipeline start using the settings */
    audio_ctrl_init(appconfMIC_GAIN_DB_DEFAULT, appconfMIC_MODE_DEFAULT);
#endif


#if appconfDEVICE_CTRL_SPI
    device_control_t *device_control_ctx[1] = {device_control_spi_ctx}; 

#if ON_TILE(0)
    gpio_servicer_start(device_control_gpio_ctx, device_control_ctx, 1 );

    servicer_t servicer_stats;
    stats_servicer_init(&servicer_stats);

    servicer_register_ctx_t stats_reg_ctx = {
        &servicer_stats,
        device_control_ctx,
        1,
        NULL
    };

    xTaskCreate(
        stats_servicer,
        "Stats servicer",
        RTOS_THREAD_STACK_SIZE(stats_servicer),
        &stats_reg_ctx,
        appconfDEVICE_CONTROL_SPI_PRIORITY,
        NULL
    );

    servicer_t dfu_servicer_ctx;
    dfu_servicer_init(&dfu_servicer_ctx);
    
    servicer_register_ctx_t dfu_servicer_reg_ctx = {
        &dfu_servicer_ctx,
        device_control_ctx,
        1,
        NULL
    };

    xTaskCreate(
        dfu_servicer,
        "dfu servicer",
        RTOS_THREAD_STACK_SIZE(dfu_servicer),
        &dfu_servicer_reg_ctx,
        appconfDEVICE_CONTROL_SPI_PRIORITY,
        NULL
    );

    xTaskCreate(
        device_control_ready_task,
        "dc ready",
        RTOS_THREAD_STACK_SIZE(device_control_ready_task),
        device_control_spi_ctx,
        appconfDEVICE_CONTROL_SPI_PRIORITY,
        NULL
    );
#endif

#if ON_TILE(MICARRAY_TILE_NO)
    servicer_t servicer_audio;
    audio_servicer_init(&servicer_audio);

    servicer_register_ctx_t audio_reg_ctx = {
        &servicer_audio,
        device_control_ctx,
        1,
        NULL
    };

    xTaskCreate(
        audio_servicer,
        "Audio servicer",
        RTOS_THREAD_STACK_SIZE(audio_servicer),
        &audio_reg_ctx,
        appconfDEVICE_CONTROL_SPI_PRIORITY,
        NULL
    );
#endif

#if ON_TILE(WS2812_TILE_NO)
    servicer_t servicer_led_ring;
    led_ring_servicer_init(&servicer_led_ring);
    
    servicer_register_ctx_t led_ring_reg_ctx = {
        &servicer_led_ring,
        device_control_ctx,
        1,
        ws2812_ctx
    };
    
    xTaskCreate(
        led_ring_servicer,
        "LED-Ring servicer",
        RTOS_THREAD_STACK_SIZE(led_ring_servicer),
        &led_ring_reg_ctx,
        appconfDEVICE_CONTROL_SPI_PRIORITY,
        NULL
    );
#endif

#endif


#if appconfI2S_ENABLED && ON_TILE(I2S_TILE_NO) && (I2S_TILE_NO != appconfAUDIO_PIPELINE_OUTPUT_TILE_NO)
    xTaskCreate(
        audio_output_task,
        "audio output",
        RTOS_THREAD_STACK_SIZE(audio_output_task),
        NULL,
        appconfI2S_RPC_PRIORITY,
        NULL
    );
#endif

#if ON_TILE(SPEAKER_PIPELINE_TILE_NO)
    ref_input_queue = rtos_osal_malloc( sizeof(rtos_osal_queue_t) );
    rtos_osal_queue_create(ref_input_queue, NULL, 2, sizeof(ref_frame_t));
    speaker_pipeline_init(NULL, NULL);
#endif

    audio_pipeline_init(NULL, NULL);
    
    init_watchdog();

    mem_analysis();
}

void vApplicationMinimalIdleHook(void)
{
    /* No printing here: it would run on every idle pass of every core */
    asm volatile("waiteu");
}

static void tile_common_init(chanend_t c)
{
    platform_init(c);
    chanend_free(c);

    xTaskCreate((TaskFunction_t) startup_task,
                "startup_task",
                RTOS_THREAD_STACK_SIZE(startup_task),
                NULL,
                appconfSTARTUP_TASK_PRIORITY,
                NULL);

    rtos_printf("start scheduler on tile %d\n", THIS_XCORE_TILE);
    vTaskStartScheduler();
}

#if ON_TILE(0)
void main_tile0(chanend_t c0, chanend_t c1, chanend_t c2, chanend_t c3)
{
    (void) c0;
    (void) c2;
    (void) c3;

    tile_common_init(c1);
}
#endif

#if ON_TILE(1)
void main_tile1(chanend_t c0, chanend_t c1, chanend_t c2, chanend_t c3)
{
    (void) c1;
    (void) c2;
    (void) c3;

    tile_common_init(c0);
}
#endif
