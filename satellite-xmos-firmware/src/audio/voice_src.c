#include <stdint.h>

#include "src.h"
#include "src_low_level.h"

#include "audio/voice_src.h"

#if SRC_FF3V_FIR_NUM_PHASES != 3 || SRC_FF3V_FIR_TAPS_PER_PHASE != 24
#error The VPU kernels are for lib_src's 3 x 24 tap voice filter
#endif

/*
 * lib_src's scalar voice filter computes y = sum(c * x) * comp / 2^(31 + q).
 * The VPU kernels compute sum(round(x * c' / 2^30)), so with the input halved
 * and the result doubled c' = c * comp / 2^(1 + q). Halving the input leaves
 * the headroom for the filter's overshoot past full scale, which then
 * saturates in sat32() instead of wrapping in the kernel.
 */
static int32_t us_coefs[3][24] __attribute__((aligned(8)));
static int32_t ds_coefs[3][24] __attribute__((aligned(8)));

void voice_src_init(void)
{
    for (unsigned p = 0; p < 3; p++) {
        for (unsigned k = 0; k < 24; k++) {
            const int64_t c = src_ff3v_fir_coefs[p][k];
            us_coefs[p][k] = (int32_t)((c * src_ff3v_fir_comp_us) >> (1 + src_ff3v_fir_comp_q_us));
            ds_coefs[p][k] = (int32_t)((c * src_ff3v_fir_comp_ds) >> (1 + src_ff3v_fir_comp_q_ds));
        }
    }
}

static inline int32_t sat32(int64_t v)
{
    if (v > INT32_MAX) {
        return INT32_MAX;
    }
    if (v < -INT32_MAX) {
        return -INT32_MAX;
    }
    return (int32_t)v;
}

void voice_src_us3(voice_src_us3_t *s, int32_t in, int32_t out[3])
{
    /* Same phase order as src_us3_voice_input_sample() / _get_next_sample() */
    out[0] = sat32((int64_t)fir_s32_24t(s->state, us_coefs[2], in >> 1) * 2);
    out[1] = sat32((int64_t)conv_s32_24t(s->state, us_coefs[1]) * 2);
    out[2] = sat32((int64_t)conv_s32_24t(s->state, us_coefs[0]) * 2);
}

int32_t voice_src_ds3(voice_src_ds3_t *s, const int32_t in[3])
{
    int64_t acc = fir_s32_24t(s->state[0], ds_coefs[0], in[0] >> 1);
    acc += fir_s32_24t(s->state[1], ds_coefs[1], in[1] >> 1);
    acc += fir_s32_24t(s->state[2], ds_coefs[2], in[2] >> 1);
    return sat32(acc * 2);
}
