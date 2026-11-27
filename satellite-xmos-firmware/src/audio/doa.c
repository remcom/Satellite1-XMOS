#include <math.h>
#include <string.h>

#include "xmath/xmath.h"

#include "audio/doa.h"
#include "audio/mic_frontend.h"

#define DOA_PI          3.14159265358979f

#if DOA_FRAME_SIZE > DOA_FFT_N
#error DOA_FFT_N must hold a whole frame
#endif

#if 4 % MIC_HPF_CHANNELS != 0 || DOA_SAMPLE_RATE != MIC_HPF_SAMPLE_RATE
#error The detector high-pass must cover all four mics at the DOA sample rate
#endif

#if DOA_BIN_LO < 1 || DOA_BIN_HI >= DOA_FFT_N / 2
#error DOA band must stay clear of DC and Nyquist (packed together in bin 0 of the real FFT)
#endif

enum { CH_N = 0, CH_S = 1, CH_E = 2, CH_W = 3 };
enum { PAIR_NS = 0, PAIR_EW = 1 };

void doa_init(doa_state_t *state)
{
    memset(state, 0, sizeof(*state));
    for (unsigned i = 0; i < 4 / MIC_HPF_CHANNELS; i++) {
        mic_hpf_init(&state->energy_hpf[i]);
    }
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
    state->playback_hangover_frames = DOA_PLAYBACK_HANGOVER_FRAMES;
    state->frames_since_playback = DOA_PLAYBACK_HANGOVER_FRAMES;
}

void doa_set_playback_hangover(doa_state_t *state, unsigned frames)
{
    state->playback_hangover_frames = frames;
    if (state->frames_since_playback > frames) {
        state->frames_since_playback = frames;
    }
}

/* b * conj(a), whose correlation peaks at t_b - t_a */
static doa_complex_t cross_product(doa_complex_t a, doa_complex_t b)
{
    const doa_complex_t g = {
        b.re * a.re + b.im * a.im,
        b.im * a.re - b.re * a.im,
    };
    return g;
}

/* g scaled to magnitude weight (PHAT, weighted) */
static doa_complex_t phat(doa_complex_t g, float weight)
{
    const float mag = sqrtf(g.re * g.re + g.im * g.im);
    if (mag > 1e-30f) {
        const float scale = weight / mag;
        g.re *= scale;
        g.im *= scale;
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

typedef struct {
    bfp_s32_t frame;                        /* the FFT works in place: spectrum points into it */
    const bfp_complex_s32_t *spectrum;
    float scale;    /* mantissa to full scale (1.0 = a full scale input sample) */
} doa_spectrum_t;

static doa_complex_t spectrum_bin(const doa_spectrum_t *x, unsigned k)
{
    const doa_complex_t v = {
        (float)x->spectrum->data[k].re * x->scale,
        (float)x->spectrum->data[k].im * x->scale,
    };
    return v;
}

/*
 * Real FFT of each windowed mic on the VPU, into state->fft_buf. Every
 * spectrum has its own block exponent, folded into the scale so the noise
 * cross spectra of different frames add up.
 */
static void mic_spectra(doa_state_t *state, const int32_t *mics, doa_spectrum_t spectrum[4])
{
    for (unsigned ch = 0; ch < 4; ch++) {
        int32_t *buf = state->fft_buf[ch];
        vect_s32_mul(buf, &mics[ch * DOA_FRAME_SIZE], state->window_q30, DOA_FRAME_SIZE, 0, 0);
        memset(&buf[DOA_FRAME_SIZE], 0, (DOA_FFT_N - DOA_FRAME_SIZE) * sizeof(int32_t));
        bfp_s32_init(&spectrum[ch].frame, buf, 0, DOA_FFT_N, 1);
        spectrum[ch].spectrum = bfp_fft_forward_mono(&spectrum[ch].frame);
        spectrum[ch].scale = ldexpf(1.0f, spectrum[ch].spectrum->exp - 31);
    }
}

static float bin_power(const doa_spectrum_t spectrum[4], unsigned k)
{
    float p = 0.0f;
    for (unsigned ch = 0; ch < 4; ch++) {
        const doa_complex_t v = spectrum_bin(&spectrum[ch], k);
        p += v.re * v.re + v.im * v.im;
    }
    return p;
}

/* A frame without speech or echo: fold it into the noise cross spectra */
static void update_noise(doa_state_t *state, const doa_spectrum_t spectrum[4])
{
    const float b = (state->noise_frames < DOA_NOISE_AVG_FRAMES) ?
                    1.0f / (float)(state->noise_frames + 1) : 1.0f / (float)DOA_NOISE_AVG_FRAMES;
    for (unsigned i = 0; i < DOA_BIN_COUNT; i++) {
        const unsigned k = DOA_BIN_LO + i;
        const doa_complex_t g_ns = cross_product(spectrum_bin(&spectrum[CH_N], k), spectrum_bin(&spectrum[CH_S], k));
        const doa_complex_t g_ew = cross_product(spectrum_bin(&spectrum[CH_E], k), spectrum_bin(&spectrum[CH_W], k));
        doa_complex_t *n_ns = &state->noise_cross[PAIR_NS][i];
        doa_complex_t *n_ew = &state->noise_cross[PAIR_EW][i];
        n_ns->re += b * (g_ns.re - n_ns->re);
        n_ns->im += b * (g_ns.im - n_ns->im);
        n_ew->re += b * (g_ew.re - n_ew->re);
        n_ew->im += b * (g_ew.im - n_ew->im);
        state->noise_power[i] += b * (bin_power(spectrum, k) - state->noise_power[i]);
    }
    if (state->noise_frames < UINT32_MAX) {
        state->noise_frames++;
    }
}

/* Weight of a bin: 0 at the noise power, rising to 1 ~10 dB above it */
static float bin_weight(const doa_state_t *state, unsigned i, float power)
{
    if (state->noise_frames < DOA_NOISE_MIN_FRAMES) {
        return 1.0f;
    }
    const float w = (power / (state->noise_power[i] * DOA_NOISE_WEIGHT_OFFSET + 1e-30f) - 1.0f) /
                    DOA_NOISE_WEIGHT_RANGE;
    return (w < 0.0f) ? 0.0f : (w > 1.0f) ? 1.0f : w;
}

static int estimate(doa_state_t *state, const doa_spectrum_t spectrum[4], int restart)
{
    const float alpha = restart ? 0.0f : DOA_SMOOTHING;
    float norm_ns = 0.0f;
    float norm_ew = 0.0f;
    for (unsigned i = 0; i < DOA_BIN_COUNT; i++) {
        const unsigned k = DOA_BIN_LO + i;
        const float weight = bin_weight(state, i, bin_power(spectrum, k));
        /* peaks at t_S - t_N: positive towards North */
        doa_complex_t x_ns = cross_product(spectrum_bin(&spectrum[CH_N], k), spectrum_bin(&spectrum[CH_S], k));
        /* peaks at t_W - t_E: positive towards East */
        doa_complex_t x_ew = cross_product(spectrum_bin(&spectrum[CH_E], k), spectrum_bin(&spectrum[CH_W], k));
        x_ns.re -= state->noise_cross[PAIR_NS][i].re;
        x_ns.im -= state->noise_cross[PAIR_NS][i].im;
        x_ew.re -= state->noise_cross[PAIR_EW][i].re;
        x_ew.im -= state->noise_cross[PAIR_EW][i].im;
        const doa_complex_t g_ns = phat(x_ns, weight);
        const doa_complex_t g_ew = phat(x_ew, weight);
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
    /*
     * Reuse FFT storage before estimate() overwrites it. Keep the channels
     * packed at FRAME_SIZE here (the FFT later uses FFT_N strides). Private
     * filters leave the raw mics and the configurable audio-path HPF alone:
     * no extra audio filtering, beam delay, or change to the PHAT input.
     */
    int32_t *filtered = &state->fft_buf[0][0];
    memcpy(filtered, mics, 4 * DOA_FRAME_SIZE * sizeof(*mics));
    for (unsigned i = 0; i < 4 / MIC_HPF_CHANNELS; i++) {
        mic_hpf_process(&state->energy_hpf[i],
                        &filtered[i * MIC_HPF_CHANNELS * DOA_FRAME_SIZE], DOA_FRAME_SIZE);
    }
    const float energy = audio_mean_square_s32(filtered, 4 * DOA_FRAME_SIZE);

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
    } else if (state->frames_since_playback < state->playback_hangover_frames) {
        state->frames_since_playback++;
    }
    const int echo = state->frames_since_playback < state->playback_hangover_frames;

    state->result.updated = 0;
    doa_spectrum_t spectrum[4];
    if (!echo && energy > DOA_MIN_ENERGY) {
        mic_spectra(state, mics, spectrum);
        if (!speech) {
            update_noise(state, spectrum);
        }
    }
    if (speech && !echo) {
        if (estimate(state, spectrum, state->frames_since_speech >= DOA_RESET_FRAMES)) {
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
