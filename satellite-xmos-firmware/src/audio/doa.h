#pragma once

#include <stdint.h>

/*
 * Direction-of-arrival estimation for the Satellite1 4-mic ring.
 *
 * GCC-PHAT is computed for the North/South and East/West pairs. The azimuth is
 * atan2 of the two time differences, so the ring diameter cancels out and does
 * not need to be known; it only bounds the lag search (DOA_MAX_LAG_SAMPLES).
 *
 * Azimuth convention: 0 degrees points at the North mic, 90 at the East mic,
 * using the mic labels of the BSP (SATELLITE1.cmake).
 */

#define DOA_FRAME_SIZE          240
#define DOA_SAMPLE_RATE         16000
#define DOA_FFT_N               256
#define DOA_INTERP              2   /* correlation is evaluated at 1/DOA_INTERP sample steps */
#define DOA_IFFT_N              (DOA_FFT_N * DOA_INTERP)
#define DOA_MAX_LAG_SAMPLES     8   /* covers mic spacings up to ~17 cm */
#define DOA_BAND_LO_HZ          300
#define DOA_BAND_HI_HZ          3500
#define DOA_SPEECH_SNR          4.0f    /* frame energy over noise floor (~6 dB) to count as speech */
#define DOA_MIN_ENERGY          1e-9f   /* ~-90 dBFS; quieter frames are ignored */
#define DOA_NOISE_FLOOR_RISE    1.002f  /* per-frame noise floor rise (~0.6 dB/s) */
#define DOA_SMOOTHING           0.6f    /* weight of the previous cross spectrum */
#define DOA_RESET_FRAMES        67      /* non-speech frames (~1 s) after which smoothing restarts */
#define DOA_MIN_CONFIDENCE      0.3f
#define DOA_MIN_TDOA_SAMPLES    0.25f   /* smaller TDOAs mean the source is above the ring */
#define DOA_HOLD_FRAMES         33      /* a result stays valid for ~0.5 s */
#define DOA_PLAYBACK_HANGOVER_FRAMES 27 /* ~400 ms of speaker tail and room reverb after playback stops */

typedef struct {
    float re;
    float im;
} doa_complex_t;

typedef struct {
    uint16_t azimuth_deg;   /* 0..359 */
    uint8_t confidence;     /* 0..255 */
    uint8_t valid;          /* a result was produced within DOA_HOLD_FRAMES */
    uint8_t updated;        /* a result was produced this frame */
} doa_result_t;

typedef struct {
    doa_complex_t twiddle[DOA_IFFT_N / 2];
    float window[DOA_FRAME_SIZE];
    doa_complex_t cross[2][DOA_FFT_N / 2 + 1];   /* smoothed PHAT cross spectra: NS, EW */
    doa_complex_t work[DOA_IFFT_N];
    float noise_floor;
    uint32_t frames_since_speech;
    uint32_t frames_since_result;
    uint32_t frames_since_playback;
    doa_result_t result;
} doa_state_t;

void doa_init(doa_state_t *state);

/**
 * Processes one frame.
 *
 * \param mics            4 channels (North, South, East, West) of DOA_FRAME_SIZE samples
 * \param playback_active non-zero while the speaker plays audio; the echo would
 *                        otherwise steer the estimate towards the speaker. The
 *                        estimate stays gated for DOA_PLAYBACK_HANGOVER_FRAMES
 *                        after playback stops.
 * \return                the current result (also kept in state->result)
 */
const doa_result_t *doa_process(doa_state_t *state,
                                const int32_t *mics,
                                int playback_active);

/* Exposed for unit tests. */
void doa_fft(doa_complex_t *data, unsigned n, const doa_complex_t *twiddle, int inverse);
