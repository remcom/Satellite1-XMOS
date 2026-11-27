#pragma once
/*
 * Experiment (simulator only, float): variants of the firmware's GCC-PHAT DOA
 * (satellite-xmos-firmware/src/audio/doa.c), to find why it points at an
 * interferer while the talker is louder. Same band, frame, window, gating and
 * checks as doa.c; the variants change the per bin weighting and smoothing.
 */
#include <stdint.h>

#define DX_FRAME    240
#define DX_FFT      256
#define DX_BIN_LO   5       /* 300 Hz */
#define DX_BIN_HI   48      /* 3000 Hz */
#define DX_BINS     (DX_BIN_HI - DX_BIN_LO + 1)

typedef enum {
    DX_W_PHAT = 0,      /* every bin weighs 1 (doa.c) */
    DX_W_SNR,           /* PHAT, weighted by each bin's speech presence over its noise floor */
    DX_W_BETA,          /* |G|^(1-beta): beta 1 is PHAT, 0 plain cross correlation */
    DX_W_CSD,           /* the noise cross spectrum (non-speech frames) subtracted before PHAT, SNR weighted */
} dx_weight_t;

typedef struct {
    float smoothing;        /* weight of the previous cross spectrum (doa.c: 0.95) */
    dx_weight_t weight;
    float beta;             /* DX_W_BETA */
    float snr_floor_rise;   /* DX_W_SNR: per frame rise of the per bin noise floor */
    int frame_gate_snr;     /* DX_W_SNR: also count a frame as speech only if its weighted bins carry enough */
} dx_cfg_t;

typedef struct {
    dx_cfg_t cfg;
    float hpf_x[4], hpf_y[4];
    float noise_floor;
    unsigned frames_since_speech;
    float bin_floor[DX_BINS];
    float cross_re[2][DX_BINS], cross_im[2][DX_BINS];
    float noise_re[2][DX_BINS], noise_im[2][DX_BINS], noise_pow[DX_BINS];  /* DX_W_CSD */
    unsigned noise_frames;
    float window[DX_FRAME];
    /* result */
    unsigned azimuth_deg;
    float confidence;
    int updated;
    float weight_sum;       /* this frame's summed bin weight, for diagnostics */
} dx_state_t;

void dx_init(dx_state_t *s, const dx_cfg_t *cfg);
/* mics: N, S, E, W, DX_FRAME samples each (float, full scale 1) */
void dx_process(dx_state_t *s, const float mics[4][DX_FRAME]);
