#pragma once

#include <stddef.h>
#include <stdint.h>

/* Shared 100 Hz high-pass, below the wake word and speech band. */
#define MIC_HPF_CUTOFF_HZ                   100
#define MIC_HPF_SAMPLE_RATE                 16000
#define MIC_HPF_CHANNELS                    2

/* Second order Butterworth high-pass per channel, coefficients Q28. */
typedef struct {
    int32_t b0;
    int32_t b1;
    int32_t b2;
    int32_t a1;
    int32_t a2;
    int32_t x1[MIC_HPF_CHANNELS];
    int32_t x2[MIC_HPF_CHANNELS];
    int32_t y1[MIC_HPF_CHANNELS];
    int32_t y2[MIC_HPF_CHANNELS];
} mic_hpf_t;

void mic_hpf_init(mic_hpf_t *hpf);
/** Clears the filter history, e.g. before the filter is used again after a pause. */
void mic_hpf_reset(mic_hpf_t *hpf);
/**
 * Filters MIC_HPF_CHANNELS consecutive channels of frame_count samples in
 * place, saturating to +-INT32_MAX.
 */
void mic_hpf_process(mic_hpf_t *hpf, int32_t *samples, size_t frame_count);
