#include <math.h>

#include "xmath/xmath.h"

#include "audio/wake_agc.h"

void wake_agc_init(wake_agc_t *state)
{
    agc_init(&state->agc, &WAKE_AGC_PROFILE);
    state->md = agc_meta_data_init();
    state->md.aec_ref_power = AGC_META_DATA_NO_AEC;
    state->md.aec_corr_factor = AGC_META_DATA_NO_AEC;
}

void wake_agc_process(wake_agc_t *state, int32_t *samples, float_s32_t vnr)
{
    state->md.vnr_flag = vnr;
    /* agc_api.h: input and output may be the same buffer */
    agc_process_frame(&state->agc, samples, samples, &state->md);
    /* Back from the wake word engine's level to this channel's, for the host's gain */
    vect_s32_shr(samples, samples, AGC_FRAME_ADVANCE, WAKE_AGC_HOST_GAIN_SHIFT);
}

float wake_agc_gain_db(const wake_agc_t *state)
{
    return 20.0f * log10f(float_s32_to_f32(state->agc.config.gain));
}
