#include <math.h>
#include <string.h>

#include "audio/doa.h"

#define DOA_PI          3.14159265358979f
#define DOA_SAMPLE_SCALE (1.0f / 2147483648.0f)

#define DOA_BIN_LO  ((DOA_BAND_LO_HZ * DOA_FFT_N + DOA_SAMPLE_RATE - 1) / DOA_SAMPLE_RATE)
#define DOA_BIN_HI  ((DOA_BAND_HI_HZ * DOA_FFT_N) / DOA_SAMPLE_RATE)

#if DOA_FRAME_SIZE > DOA_FFT_N
#error DOA_FFT_N must hold a whole frame
#endif

#if DOA_BIN_HI >= DOA_FFT_N / 2
#error DOA band must stay below Nyquist
#endif

enum { CH_N = 0, CH_S = 1, CH_E = 2, CH_W = 3 };
enum { PAIR_NS = 0, PAIR_EW = 1 };

void doa_fft(doa_complex_t *x, unsigned n, const doa_complex_t *twiddle, int inverse)
{
    /* bit-reversal permutation */
    for (unsigned i = 1, j = 0; i < n; i++) {
        unsigned bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            doa_complex_t t = x[i];
            x[i] = x[j];
            x[j] = t;
        }
    }

    for (unsigned len = 2; len <= n; len <<= 1) {
        const unsigned half = len >> 1;
        const unsigned step = DOA_IFFT_N / len;
        for (unsigned i = 0; i < n; i += len) {
            for (unsigned k = 0; k < half; k++) {
                doa_complex_t w = twiddle[k * step];
                if (inverse) {
                    w.im = -w.im;
                }
                doa_complex_t *u = &x[i + k];
                doa_complex_t *v = &x[i + k + half];
                const float vr = v->re * w.re - v->im * w.im;
                const float vi = v->re * w.im + v->im * w.re;
                v->re = u->re - vr;
                v->im = u->im - vi;
                u->re += vr;
                u->im += vi;
            }
        }
    }
}

void doa_init(doa_state_t *state)
{
    memset(state, 0, sizeof(*state));
    for (unsigned k = 0; k < DOA_IFFT_N / 2; k++) {
        const float phase = -2.0f * DOA_PI * (float)k / (float)DOA_IFFT_N;
        state->twiddle[k].re = cosf(phase);
        state->twiddle[k].im = sinf(phase);
    }
    for (unsigned i = 0; i < DOA_FRAME_SIZE; i++) {
        state->window[i] = 0.5f - 0.5f * cosf(2.0f * DOA_PI * (float)i / (float)(DOA_FRAME_SIZE - 1));
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
        g.re /= mag;
        g.im /= mag;
    } else {
        g.re = 0.0f;
        g.im = 0.0f;
    }
    return g;
}

/* Lag (in samples) of the correlation peak within the search range, and its normalised height. */
static float find_peak(const doa_complex_t *corr, int use_imag, float norm, float *confidence)
{
    const int max_lag = DOA_MAX_LAG_SAMPLES * DOA_INTERP;
    int best_lag = 0;
    float best = -1e30f;

#define CORR_AT(lag) (use_imag ? corr[((lag) + DOA_IFFT_N) % DOA_IFFT_N].im \
                               : corr[((lag) + DOA_IFFT_N) % DOA_IFFT_N].re)

    for (int lag = -max_lag; lag <= max_lag; lag++) {
        const float v = CORR_AT(lag);
        if (v > best) {
            best = v;
            best_lag = lag;
        }
    }

    /* parabolic interpolation around the peak */
    const float ym = CORR_AT(best_lag - 1);
    const float yp = CORR_AT(best_lag + 1);
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
#undef CORR_AT

    *confidence = (norm > 0.0f) ? best / norm : 0.0f;
    return ((float)best_lag + delta) / (float)DOA_INTERP;
}

static int estimate(doa_state_t *state, const int32_t *mics, int restart)
{
    const int32_t *ch_n = &mics[CH_N * DOA_FRAME_SIZE];
    const int32_t *ch_s = &mics[CH_S * DOA_FRAME_SIZE];
    const int32_t *ch_e = &mics[CH_E * DOA_FRAME_SIZE];
    const int32_t *ch_w = &mics[CH_W * DOA_FRAME_SIZE];

    /* Two real signals per complex FFT: a = N + jS, b = E + jW */
    doa_complex_t *a = &state->work[0];
    doa_complex_t *b = &state->work[DOA_FFT_N];
    for (unsigned i = 0; i < DOA_FRAME_SIZE; i++) {
        const float w = state->window[i] * DOA_SAMPLE_SCALE;
        a[i].re = (float)ch_n[i] * w;
        a[i].im = (float)ch_s[i] * w;
        b[i].re = (float)ch_e[i] * w;
        b[i].im = (float)ch_w[i] * w;
    }
    for (unsigned i = DOA_FRAME_SIZE; i < DOA_FFT_N; i++) {
        a[i].re = a[i].im = 0.0f;
        b[i].re = b[i].im = 0.0f;
    }
    doa_fft(a, DOA_FFT_N, state->twiddle, 0);
    doa_fft(b, DOA_FFT_N, state->twiddle, 0);

    const float alpha = restart ? 0.0f : DOA_SMOOTHING;
    for (unsigned k = 0; k <= DOA_FFT_N / 2; k++) {
        doa_complex_t *c_ns = &state->cross[PAIR_NS][k];
        doa_complex_t *c_ew = &state->cross[PAIR_EW][k];
        if (k < DOA_BIN_LO || k > DOA_BIN_HI) {
            c_ns->re = c_ns->im = 0.0f;
            c_ew->re = c_ew->im = 0.0f;
            continue;
        }
        const unsigned nk = (DOA_FFT_N - k) & (DOA_FFT_N - 1);
        /* X1 = (Z[k] + conj(Z[N-k])) / 2, X2 = (Z[k] - conj(Z[N-k])) / 2j */
        const doa_complex_t xn = { 0.5f * (a[k].re + a[nk].re),  0.5f * (a[k].im - a[nk].im) };
        const doa_complex_t xs = { 0.5f * (a[k].im + a[nk].im), -0.5f * (a[k].re - a[nk].re) };
        const doa_complex_t xe = { 0.5f * (b[k].re + b[nk].re),  0.5f * (b[k].im - b[nk].im) };
        const doa_complex_t xw = { 0.5f * (b[k].im + b[nk].im), -0.5f * (b[k].re - b[nk].re) };

        const doa_complex_t g_ns = phat(xn, xs);   /* peaks at t_S - t_N: positive towards North */
        const doa_complex_t g_ew = phat(xe, xw);   /* peaks at t_W - t_E: positive towards East */
        c_ns->re = alpha * c_ns->re + (1.0f - alpha) * g_ns.re;
        c_ns->im = alpha * c_ns->im + (1.0f - alpha) * g_ns.im;
        c_ew->re = alpha * c_ew->re + (1.0f - alpha) * g_ew.re;
        c_ew->im = alpha * c_ew->im + (1.0f - alpha) * g_ew.im;
    }

    /*
     * Both correlations with one zero-padded complex IFFT: z = r_ns + j r_ew,
     * where r_ns and r_ew are real because their spectra are Hermitian.
     */
    doa_complex_t *z = state->work;
    memset(z, 0, sizeof(state->work));
    float norm_ns = 0.0f;
    float norm_ew = 0.0f;
    for (unsigned k = DOA_BIN_LO; k <= DOA_BIN_HI; k++) {
        const doa_complex_t c_ns = state->cross[PAIR_NS][k];
        const doa_complex_t c_ew = state->cross[PAIR_EW][k];
        z[k].re = c_ns.re - c_ew.im;
        z[k].im = c_ns.im + c_ew.re;
        z[DOA_IFFT_N - k].re = c_ns.re + c_ew.im;
        z[DOA_IFFT_N - k].im = c_ew.re - c_ns.im;
        norm_ns += 2.0f * sqrtf(c_ns.re * c_ns.re + c_ns.im * c_ns.im);
        norm_ew += 2.0f * sqrtf(c_ew.re * c_ew.re + c_ew.im * c_ew.im);
    }
    doa_fft(z, DOA_IFFT_N, state->twiddle, 1);

    float conf_ns;
    float conf_ew;
    const float tau_ns = find_peak(z, 0, norm_ns, &conf_ns);
    const float tau_ew = find_peak(z, 1, norm_ew, &conf_ew);
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
    float energy = 0.0f;
    for (unsigned i = 0; i < 4 * DOA_FRAME_SIZE; i++) {
        const float s = (float)mics[i] * DOA_SAMPLE_SCALE;
        energy += s * s;
    }
    energy /= (float)(4 * DOA_FRAME_SIZE);

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
