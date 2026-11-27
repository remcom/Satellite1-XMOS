#include "audio/echo_gate.h"

void echo_gate_init(echo_gate_t *gate)
{
    gate->floor = 0.0f;
}

int echo_gate_update(echo_gate_t *gate, int playback, float in_energy, float out_energy)
{
    if (!playback) {
        return 0;
    }
    if (in_energy < ECHO_GATE_MIN_ENERGY) {
        return 1;
    }
    const float ratio = out_energy / in_energy;
    if (gate->floor <= 0.0f || ratio < gate->floor) {
        gate->floor = ratio;
    } else {
        gate->floor *= ECHO_GATE_FLOOR_RISE;
        if (gate->floor > 1.0f) {
            gate->floor = 1.0f;
        }
    }
    const int converged = gate->floor < ECHO_GATE_MIN_ERLE;
    const int near_end = ratio > gate->floor * ECHO_GATE_NEAR_END_RATIO;
    return !(converged && near_end);
}
