#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Acoustic model for the voice pipeline simulation: a shoebox room (image
 * source method) with the Satellite1 4-mic ring and its speaker, plus the test
 * signals. Everything is float at 16 kHz, full scale = 1.0.
 */

#define SIM_FS          16000
#define SIM_MICS        4       /* N, S, E, W as in mic_frontend.h */
#define SIM_RIR_LEN     5600    /* 350 ms */

typedef struct {
    float *x;
    size_t n;
} asig_t;

typedef struct {
    float room[3];          /* m */
    float device[3];        /* centre of the mic ring */
    float beta;             /* wall reflection coefficient */
} room_t;

asig_t sig_alloc(size_t n);
void sig_free(asig_t *s);
void sig_add(asig_t *dst, const asig_t *src, size_t offset, float gain);
float sig_rms(const asig_t *s, size_t start, size_t len);
void sig_scale_to_rms(asig_t *s, float rms);

/** Loads a 16 kHz mono 16 bit wav. Exits on error. */
asig_t sig_load_wav(const char *path);

/** RT60 (Sabine) of the room. */
float room_rt60(const room_t *room);

/**
 * Impulse response from a point source to mic m (mic_idx order). Source
 * position relative to the device centre. rir must hold SIM_RIR_LEN samples.
 */
void room_rir(const room_t *room, const float src_rel[3], unsigned mic, float *rir);

/** Source position relative to the device: azimuth (0 = North mic, 90 = East), distance, height above the ring. */
void room_source_pos(float azimuth_deg, float distance, float height, float pos[3]);

/** out[m] += conv(src, rir to mic m), for all four mics. */
void room_render(const room_t *room, const float src_rel[3], const asig_t *src, asig_t out[SIM_MICS]);

/** Deterministic xorshift noise in [-1, 1). */
float sim_rand(uint32_t *state);

/**
 * Synthetic stereo music: three panned harmonic instruments and a centre
 * percussion part. Every section_s seconds the panning of each instrument is
 * redrawn, which changes the correlation between L and R. width 0 = mono.
 */
void music_synth(asig_t *left, asig_t *right, float section_s, float width, uint32_t seed);

/** Pink noise. */
void noise_synth(asig_t *s, uint32_t seed);

/** Small speaker: soft saturation with ~2% THD at full drive. */
void speaker_nonlinearity(asig_t *s, float drive);
