#pragma once

#include <stdint.h>

/*
 * Decides, during playback, whether the echo cancelled mics are dominated by
 * the talker (the DOA may run) or by residual echo (it must not, or it would
 * point at the speaker). Used by the aec4 build, whose DOA runs after the AEC.
 *
 * Per frame, ratio = AEC output energy / AEC input energy over all mics.
 * With only the speaker playing that is the residual echo, 1 / ERLE; the
 * gate tracks its minimum (the converged ERLE) like a noise floor: it drops
 * at once and rises slowly. A talker adds energy the AEC does not remove, so
 * the ratio rises above that floor. The DOA may run when
 *   - the AEC has converged: floor below ECHO_GATE_MIN_ERLE, and
 *   - this frame's ratio is ECHO_GATE_NEAR_END_RATIO above the floor,
 * i.e. the talker is ~10 dB above the residual echo.
 */
#define ECHO_GATE_MIN_ERLE          0.1f    /* 10 dB */
#define ECHO_GATE_NEAR_END_RATIO    10.0f   /* 10 dB above the residual echo */
#define ECHO_GATE_FLOOR_RISE        1.01f   /* per frame, ~2.9 dB/s: follows a changing echo path */
#define ECHO_GATE_MIN_ENERGY        1e-12f  /* below this the input is silence */

typedef struct {
    float floor;    /* minimum output/input energy ratio during playback, 0 = unknown */
} echo_gate_t;

void echo_gate_init(echo_gate_t *gate);

/**
 * \param playback      non-zero while the speaker plays (the reference is active)
 * \param in_energy     mean square of the AEC input (all mics), full scale = 1.0
 * \param out_energy    mean square of the AEC output (all mics)
 * \return              non-zero if the frame is echo and the DOA must skip it
 */
int echo_gate_update(echo_gate_t *gate, int playback, float in_energy, float out_energy);
