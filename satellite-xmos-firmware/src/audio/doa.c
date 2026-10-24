#include <math.h>
#include <string.h>

#include "xmath/xmath.h"

#include "audio/doa.h"
#include "audio/mic_frontend.h"

#define DOA_PI          3.14159265358979f
/* Spectrum mantissas are scaled to keep PHAT products and their squares well inside float range */
#define DOA_BIN_SCALE   (1.0f / 65536.0f)

#if DOA_FRAME_SIZE > DOA_FFT_N
#error DOA_FFT_N must hold a whole frame
#endif

#if DOA_BIN_LO < 1 || DOA_BIN_HI >= DOA_FFT_N / 2
#error DOA band must stay clear of DC and Nyquist (packed together in bin 0 of the real FFT)
#endif

enum { CH_N = 0, CH_S = 1, CH_E = 2, CH_W = 3 };
enum { PAIR_NS = 0, PAIR_EW = 1 };

void doa_init(doa_state_t *state)
{
    memset(state, 0, sizeof(*state));
    for (unsigned i = 0; i < DOA_BIN_COUNT; i++) {
        const float phase = 2.0f * DOA_PI * (float)(DOA_BIN_LO + i) / (float)DOA_IFFT_N;
        state->lag_step[i].re = cosf(phase);
        state->lag_step[i].im = sinf(phase);
    }
    for (unsigned i = 0; i < DOA_FRAME_SIZE; i++) {
        const float w = 0.5f - 0.5f * cosf(2.0f * DOA_PI * (float)i / (float)(DOA_FRAME_SIZE - 1));
        state->window_q30[i] = (int32_t)(w * 1073741824.0f + 0.5f);
    }
    state->frames_since_speech = DOA_RESET_FRAMES;
    state->frames_since_result = DOA_HOLD_FRAMES;
    state->frames_since_playback = DOA_PLAYBACK_HANGOVER_FRAMES;
}

/* Normalised (PHAT) b * conj(a), whose correlation peaks at t_b - t_a. */
static doa_complex_t phat(doa_complex_t a, doa_complex_t b)
{
    doa_complex_t g = {
        b.re * a.re + b.im * a.im,
        b.im * a.re - b.re * a.im,
    };
    const float mag = sqrtf(g.re * g.re + g.im * g.im);
    if (mag > 1e-30f) {
        const float inv = 1.0f / mag;
        g.re *= inv;
        g.im *= inv;
    } else {
        g.re = 0.0f;
        g.im = 0.0f;
    }
    return g;
}

void doa_correlate(const doa_state_t *state, const doa_complex_t *cross, float *corr)
{
    /*
     * Only DOA_CORR_LAGS lags of the correlation are needed, so they are summed
     * directly over the band bins instead of running a zero padded inverse FFT.
     */
    memset(corr, 0, DOA_CORR_LAGS * sizeof(float));
    for (unsigned i = 0; i < DOA_BIN_COUNT; i++) {
        const doa_complex_t c = cross[i];
        const doa_complex_t step = state->lag_step[i];
        doa_complex_t p = { 1.0f, 0.0f };   /* e^(j 2 pi k l / DOA_IFFT_N) */
        corr[DOA_CORR_HALF] += c.re;
        for (unsigned l = 1; l <= DOA_CORR_HALF; l++) {
            const float re = p.re * step.re - p.im * step.im;
            p.im = p.re * step.im + p.im * step.re;
            p.re = re;
            /* Re(c p) at lag +l, Re(c conj(p)) at lag -l */
            const float a = c.re * p.re;
            const float b = c.im * p.im;
            corr[DOA_CORR_HALF + l] += a - b;
            corr[DOA_CORR_HALF - l] += a + b;
        }
    }
}

/* Lag (in samples) of the correlation peak within the search range, and its normalised height. */
static float find_peak(const float *corr, float norm, float *confidence)
{
    const int max_lag = DOA_MAX_LAG_SAMPLES * DOA_INTERP;
    const float *c = &corr[DOA_CORR_HALF];
    int best_lag = 0;
    float best = -1e30f;

    for (int lag = -max_lag; lag <= max_lag; lag++) {
        if (c[lag] > best) {
            best = c[lag];
            best_lag = lag;
        }
    }

    /* parabolic interpolation around the peak */
    const float ym = c[best_lag - 1];
    const float yp = c[best_lag + 1];
    const float denom = ym - 2.0f * best + yp;
    float delta = 0.0f;
    if (denom < 0.0f) {
        delta = 0.5f * (ym - yp) / denom;
        if (delta > 0.5f) {
            delta = 0.5f;
        } else if (delta < -0.5f) {
            delta = -0.5f;
        }
    }

    *confidence = (norm > 0.0f) ? best / norm : 0.0f;
    return ((float)best_lag + delta) / (float)DOA_INTERP;
}

static doa_complex_t spectrum_bin(const bfp_complex_s32_t *x, unsigned k)
{
    const doa_complex_t v = {
        (float)x->data[k].re * DOA_BIN_SCALE,
        (float)x->data[k].im * DOA_BIN_SCALE,
    };
    return v;
}

static int estimate(doa_state_t *state, const int32_t *mics, int restart)
{
    /*
     * Real FFT of each windowed mic on the VPU. Every spectrum has its own
     * block exponent, which PHAT normalises away, so only the mantissas are used.
     */
    bfp_s32_t frame[4];
    const bfp_complex_s32_t *spectrum[4];
    for (unsigned ch = 0; ch < 4; ch++) {
        int32_t *buf = state->fft_buf[ch];
        vect_s32_mul(buf, &mics[ch * DOA_FRAME_SIZE], state->window_q30, DOA_FRAME_SIZE, 0, 0);
        memset(&buf[DOA_FRAME_SIZE], 0, (DOA_FFT_N - DOA_FRAME_SIZE) * sizeof(int32_t));
        bfp_s32_init(&frame[ch], buf, 0, DOA_FFT_N, 1);
        spectrum[ch] = bfp_fft_forward_mono(&frame[ch]);
    }

    const float alpha = restart ? 0.0f : DOA_SMOOTHING;
    float norm_ns = 0.0f;
    float norm_ew = 0.0f;
    for (unsigned i = 0; i < DOA_BIN_COUNT; i++) {
        const unsigned k = DOA_BIN_LO + i;
        const doa_complex_t g_ns = phat(spectrum_bin(spectrum[CH_N], k),
                                        spectrum_bin(spectrum[CH_S], k));   /* peaks at t_S - t_N: positive towards North */
        const doa_complex_t g_ew = phat(spectrum_bin(spectrum[CH_E], k),
                                        spectrum_bin(spectrum[CH_W], k));   /* peaks at t_W - t_E: positive towards East */
        doa_complex_t *c_ns = &state->cross[PAIR_NS][i];
        doa_complex_t *c_ew = &state->cross[PAIR_EW][i];
        c_ns->re = alpha * c_ns->re + (1.0f - alpha) * g_ns.re;
        c_ns->im = alpha * c_ns->im + (1.0f - alpha) * g_ns.im;
        c_ew->re = alpha * c_ew->re + (1.0f - alpha) * g_ew.re;
        c_ew->im = alpha * c_ew->im + (1.0f - alpha) * g_ew.im;
        norm_ns += sqrtf(c_ns->re * c_ns->re + c_ns->im * c_ns->im);
        norm_ew += sqrtf(c_ew->re * c_ew->re + c_ew->im * c_ew->im);
    }

    float corr[DOA_CORR_LAGS];
    float conf_ns;
    float conf_ew;
    doa_correlate(state, state->cross[PAIR_NS], corr);
    const float tau_ns = find_peak(corr, norm_ns, &conf_ns);
    doa_correlate(state, state->cross[PAIR_EW], corr);
    const float tau_ew = find_peak(corr, norm_ew, &conf_ew);
    const float confidence = (conf_ns < conf_ew) ? conf_ns : conf_ew;

    if (confidence < DOA_MIN_CONFIDENCE) {
        return 0;
    }
    const float tau_sq = tau_ns * tau_ns + tau_ew * tau_ew;
    if (tau_sq < DOA_MIN_TDOA_SAMPLES * DOA_MIN_TDOA_SAMPLES) {
        return 0;
    }
    if (tau_sq > (float)(DOA_MAX_LAG_SAMPLES * DOA_MAX_LAG_SAMPLES)) {
        return 0;
    }

    float azimuth = atan2f(tau_ew, tau_ns) * (180.0f / DOA_PI);
    if (azimuth < 0.0f) {
        azimuth += 360.0f;
    }
    state->result.azimuth_deg = (uint16_t)((unsigned)(azimuth + 0.5f) % 360);
    const float conf_scaled = confidence * 255.0f + 0.5f;
    state->result.confidence = (conf_scaled >= 255.0f) ? 255 : (uint8_t)conf_scaled;
    return 1;
}

const doa_result_t *doa_process(doa_state_t *state,
                                const int32_t *mics,
                                int playback_active)
{
    const float energy = audio_mean_square_s32(mics, 4 * DOA_FRAME_SIZE);

    /* Minimum-tracking noise floor: drops at once, rises slowly. */
    if (state->noise_floor <= 0.0f || energy < state->noise_floor) {
        state->noise_floor = energy;
    } else {
        state->noise_floor *= DOA_NOISE_FLOOR_RISE;
    }

    const int speech = (energy > DOA_MIN_ENERGY) &&
                       (energy > state->noise_floor * DOA_SPEECH_SNR);

    if (playback_active) {
        state->frames_since_playback = 0;
    } else if (state->frames_since_playback < DOA_PLAYBACK_HANGOVER_FRAMES) {
        state->frames_since_playback++;
    }
    const int echo = state->frames_since_playback < DOA_PLAYBACK_HANGOVER_FRAMES;

    state->result.updated = 0;
    if (speech && !echo) {
        if (estimate(state, mics, state->frames_since_speech >= DOA_RESET_FRAMES)) {
            state->result.updated = 1;
            state->frames_since_result = 0;
        }
        state->frames_since_speech = 0;
    } else if (state->frames_since_speech < DOA_RESET_FRAMES) {
        state->frames_since_speech++;
    }

    if (!state->result.updated && state->frames_since_result < DOA_HOLD_FRAMES) {
        state->frames_since_result++;
    }
    state->result.valid = state->frames_since_result < DOA_HOLD_FRAMES;

    return &state->result;
}
