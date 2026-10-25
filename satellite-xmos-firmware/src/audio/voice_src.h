#pragma once

#include <stdint.h>

/*
 * 16 <-> 48 kHz conversion on the VPU, with the same 72 tap filter as
 * lib_src's src_us3_voice_* / src_ds3_voice_* functions (same response and
 * delay), in about half the cycles. Outputs match those functions to within
 * ~20 LSB and saturate the same way at full scale.
 */

typedef struct {
    int32_t state[24] __attribute__((aligned(8)));
} voice_src_us3_t;

typedef struct {
    int32_t state[3][24] __attribute__((aligned(8)));
} voice_src_ds3_t;

/** Builds the VPU coefficient tables. Call once per tile before the functions below. */
void voice_src_init(void);

/** One 16 kHz sample in, three 48 kHz samples out. state starts zeroed. */
void voice_src_us3(voice_src_us3_t *s, int32_t in, int32_t out[3]);

/** Three 48 kHz samples in, one 16 kHz sample out. state starts zeroed. */
int32_t voice_src_ds3(voice_src_ds3_t *s, const int32_t in[3]);
