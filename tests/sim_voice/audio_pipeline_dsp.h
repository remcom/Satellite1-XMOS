#pragma once
/*
 * Stand-in for the firmware's audio_pipeline_dsp.h (which needs FreeRTOS).
 * The simulator builds lib_voice's AEC with room for 4 mics and 2 references
 * (build.sh generates that aec_conf.h), so one binary runs the 2-mic build
 * (2x2 or 2x1) and the aec4 build (4x1).
 */
#include "aec.h"
