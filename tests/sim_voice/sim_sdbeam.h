#pragma once
/*
 * Prototype (simulator only, float): a superdirective beamformer for the
 * Satellite1 4-mic ring, as an alternative to the delay-and-sum beams of
 * satellite-xmos-firmware/src/audio/beamformer.c. See docs/voice-simulation.md.
 *
 * STFT: 480 sample sqrt-Hann window, 240 sample hop (one pipeline frame),
 * 512 point FFT. Per bin, the target weights are the MVDR solution for a
 * spherically diffuse noise field, w = (G + mu I)^-1 d / (d^H (G + mu I)^-1 d),
 * with the smallest diagonal loading mu that keeps the white noise gain
 * (1 / |w|^2) at or above wng_min_db, which bounds how far the beam amplifies
 * mic self noise and mic mismatch. The blocking beam is the firmware's: the
 * mics phase aligned to the steering direction, weighted by
 * cos(mic azimuth - steering) / 2, which nulls the steering direction.
 *
 * Mic order and azimuth convention follow beamformer.h: N, S, E, W; 0 degrees
 * points at the North mic, 90 at the East mic. Output lags the input by one
 * frame (240 samples, 15 ms) more than the firmware beams.
 */
#include <stdint.h>

#define SDB_MICS    4
#define SDB_HOP     240
#define SDB_WIN     480
#define SDB_FFT     512
#define SDB_BINS    (SDB_FFT / 2 + 1)
/* Fixed beams sdb_process_multi() forms, at 0, 60, ... 300 degrees */
#define SDB_MULTI   6

typedef struct {
    float re[SDB_MICS][SDB_BINS];   /* w*, applied as sum_m w_m* X_m */
    float im[SDB_MICS][SDB_BINS];
    float block[SDB_MICS];          /* blocking weights (on the phase aligned mics) */
    float align_re[SDB_MICS][SDB_BINS];    /* conj(d): phase alignment to the steering */
    float align_im[SDB_MICS][SDB_BINS];
} sdb_weights_t;

typedef struct {
    float wng_min_db;
    float in_hist[SDB_MICS][SDB_WIN - SDB_HOP];  /* previous hop of each mic */
    float ola[2][SDB_WIN - SDB_HOP];             /* overlap of target, blocking */
    float spec_re[SDB_MICS][SDB_BINS];           /* this frame's mic spectra */
    float spec_im[SDB_MICS][SDB_BINS];
    sdb_weights_t w;
    sdb_weights_t w_prev;
    int fade;                                    /* crossfade from w_prev this frame */
    int steered;
    unsigned azimuth_deg;
    sdb_weights_t multi[SDB_MULTI];
    float min_wng_db;                            /* lowest white noise gain of the current weights */
} sdb_t;

void sdb_init(sdb_t *s, float wng_min_db, unsigned azimuth_deg);

/** Steers the target and blocking beams; the next frame crossfades. */
void sdb_steer(sdb_t *s, unsigned azimuth_deg);

/** Takes in one frame of the 4 mics (Q31, N S E W), computing their spectra. */
void sdb_analyse(sdb_t *s, const int32_t mics[SDB_MICS][SDB_HOP]);

/** Forms the steered target (out[0]) and blocking (out[1]) beams of the analysed frame. */
void sdb_beams(sdb_t *s, int32_t out[2][SDB_HOP]);

/**
 * Speech-band (300-3400 Hz) power of the SDB_MULTI fixed target beams of the
 * analysed frame, for picking a beam.
 */
void sdb_multi_power(const sdb_t *s, float power[SDB_MULTI]);

/** Directivity index of the current target weights at a frequency (diffuse field), in dB. */
float sdb_directivity_db(const sdb_t *s, float freq_hz);
