#pragma once
/*
 * Stand-in for the firmware's audio_pipeline_dsp.h (which needs FreeRTOS), so
 * the firmware's aec/aec_memory_pool.h can be included. The simulator builds
 * lib_aec with room for 4 mics and 2 references, so one binary runs the
 * 2-mic build (2x2 or 2x1) and the aec4 build (4x1).
 */
#include "aec_api.h"

#define AEC_MAX_Y_CHANNELS 4
#define AEC_MAX_X_CHANNELS 2
#define AEC_MAIN_FILTER_PHASES 10
#define AEC_SHADOW_FILTER_PHASES 5
