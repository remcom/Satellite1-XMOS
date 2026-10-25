// Copyright 2022-2024 XMOS LIMITED.
// This Software is subject to the terms of the XMOS Public Licence: Version 1.

#ifndef APP_CONF_H_
#define APP_CONF_H_

/* Auto-generated firmware-version file */
#include "version.h"

/* Intertile port settings */
#define appconfGPIO_T0_RPC_PORT        1
#define appconfGPIO_T1_RPC_PORT        2
#define appconfAUDIOPIPELINE_PORT      7
/* Pipeline output to the I2S tile, see audio_output_task() */
#define appconfAUDIO_OUTPUT_PORT       8


/* Application tile specifiers */
#include "platform/driver_instances.h"


/* Audio Pipeline Configuration */
#define appconfAUDIO_CLOCK_FREQUENCY            MIC_ARRAY_CONFIG_MCLK_FREQ
#define appconfAUDIO_PIPELINE_SAMPLE_RATE       16000
/* The mic front end reduces the raw mics to the channels the AEC/IC/NS/AGC pipeline processes */
#define appconfAUDIO_PIPELINE_MIC_COUNT         MIC_ARRAY_CONFIG_MIC_COUNT
#define appconfAUDIO_PIPELINE_CHANNELS          2
#define appconfAUDIO_PIPELINE_REF_CHANNELS      2

/* Mic gain in dB, applied after the AEC. Runtime adjustable via the audio servicer. */
#ifndef appconfMIC_GAIN_DB_DEFAULT
#define appconfMIC_GAIN_DB_DEFAULT              20
#endif

/* mic_mode_t, see audio/mic_frontend.h. Runtime adjustable via the audio servicer. */
#ifndef appconfMIC_MODE_DEFAULT
#define appconfMIC_MODE_DEFAULT                 0 /* MIC_MODE_PAIR_EW */
#endif

/*
 * LED ring direction indicator: the LED that sits at the North mic, and whether
 * LED indices increase towards the East mic. Needs to be confirmed on hardware.
 */
#ifndef appconfLED_RING_DOA_LED_OFFSET
#define appconfLED_RING_DOA_LED_OFFSET          0
#endif
#ifndef appconfLED_RING_DOA_CLOCKWISE
#define appconfLED_RING_DOA_CLOCKWISE           1
#endif

#ifndef appconfAUDIO_SPK_PL_SR_FACTOR
#define appconfAUDIO_SPK_PL_SR_FACTOR           3
#endif

/* If in channel sample format, appconfAUDIO_PIPELINE_FRAME_ADVANCE == MIC_ARRAY_CONFIG_SAMPLES_PER_FRAME*/
#define appconfAUDIO_PIPELINE_FRAME_ADVANCE     MIC_ARRAY_CONFIG_SAMPLES_PER_FRAME
#define appconfAUDIO_SPK_PIPELINE_FRAME_ADVANCE appconfAUDIO_SPK_PL_SR_FACTOR * appconfAUDIO_PIPELINE_FRAME_ADVANCE

/**
 * A positive delay will delay mics
 * A negative delay will delay ref
 */
#define appconfINPUT_SAMPLES_MIC_DELAY_MS        0

#ifndef appconfPIPELINE_BYPASS
#define appconfPIPELINE_BYPASS 0
#endif

#if appconfPIPELINE_BYPASS
#define appconfAUDIO_PIPELINE_SKIP_STATIC_DELAY  1
#define appconfAUDIO_PIPELINE_SKIP_AEC           1
#define appconfAUDIO_PIPELINE_SKIP_IC_AND_VNR    1
#define appconfAUDIO_PIPELINE_SKIP_NS            1
#define appconfAUDIO_PIPELINE_SKIP_AGC           1
#endif

#ifndef appconfAUDIO_PIPELINE_SKIP_STATIC_DELAY
#define appconfAUDIO_PIPELINE_SKIP_STATIC_DELAY  0
#endif

#ifndef appconfAUDIO_PIPELINE_SKIP_AEC
#define appconfAUDIO_PIPELINE_SKIP_AEC           0
#endif

#ifndef appconfAUDIO_PIPELINE_SKIP_IC_AND_VNR
#define appconfAUDIO_PIPELINE_SKIP_IC_AND_VNR    0
#endif

#ifndef appconfAUDIO_PIPELINE_SKIP_NS
#define appconfAUDIO_PIPELINE_SKIP_NS            0
#endif

#ifndef appconfAUDIO_PIPELINE_SKIP_AGC
#define appconfAUDIO_PIPELINE_SKIP_AGC           0
#endif

#ifndef appconfAUDIO_PIPELINE_STORE_IC_AUDIO 
#define appconfAUDIO_PIPELINE_STORE_IC_AUDIO     1
#endif

#ifndef appconfAUDIO_PIPELINE_STORE_NS_AUDIO
#define appconfAUDIO_PIPELINE_STORE_NS_AUDIO     1
#endif

#ifndef appconfI2S_ENABLED
#define appconfI2S_ENABLED         1
#endif

#ifndef appconfI2S_AUDIO_SAMPLE_RATE
#define appconfI2S_AUDIO_SAMPLE_RATE appconfAUDIO_PIPELINE_SAMPLE_RATE
#endif

#define appconfI2S_AUDIO_INPUTS    1
#define appconfI2S_AUDIO_OUTPUTS   2

#ifndef appconfEXTERNAL_MCLK
#define appconfEXTERNAL_MCLK       0
#endif

/* TDM output is not supported; app_conf_check.h rejects it. */
#ifndef appconfI2S_TDM_ENABLED
#define appconfI2S_TDM_ENABLED     0
#endif

#define appconfI2S_MODE_MASTER     0
#define appconfI2S_MODE_SLAVE      1
#ifndef appconfI2S_MODE
#define appconfI2S_MODE            appconfI2S_MODE_MASTER
#endif

#include "app_conf_check.h"

/* Tile running audio_pipeline_output() (the last pipeline stages) */
#define appconfAUDIO_PIPELINE_OUTPUT_TILE_NO      0

/* Task Priorities */
#define appconfSTARTUP_TASK_PRIORITY              (configMAX_PRIORITIES/2 + 5)
#define appconfAUDIO_PIPELINE_TASK_PRIORITY       (configMAX_PRIORITIES / 2)

/* Software PLL settings for mclk recovery configurations */
/* see fractions.h and register_setup.h for other pll settings */
#define PLL_RATIO                   (MIC_ARRAY_CONFIG_MCLK_FREQ / appconfI2S_AUDIO_SAMPLE_RATE)
#define PLL_CONTROL_LOOP_COUNT_INT  512  // How many refclk ticks (LRCLK) per control loop iteration. Aim for ~100Hz
#define PLL_PPM_RANGE               1000 // Max allowable diff in clk count. For the PID constants we
                                         // have chosen, this number should be larger than the number
                                         // of elements in the look up table as the clk count diff is
                                         // added to the LUT index with a multiplier of 1. Only used for INT mclkless


#endif /* APP_CONF_H_ */
