#include <math.h>
#include <string.h>

#include "sim_doa_exp.h"

#define PI_F 3.14159265358979f
#define MAX_LAG 4
#define INTERP 2
#define LAGS (MAX_LAG * INTERP + 1)

static void fft256(float *re, float *im)
{
    const unsigned n = DX_FFT;
    for (unsigned i = 1, j = 0; i < n; i++) {
        unsigned bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (unsigned len = 2; len <= n; len <<= 1) {
        const float a = -2.0f * PI_F / (float)len;
        for (unsigned i = 0; i < n; i += len) {
            for (unsigned k = 0; k < len / 2; k++) {
                const float wr = cosf(a * k), wi = sinf(a * k);
                const unsigned p = i + k, q = i + k + len / 2;
                const float xr = re[q] * wr - im[q] * wi;
                const float xi = re[q] * wi + im[q] * wr;
                re[q] = re[p] - xr; im[q] = im[p] - xi;
                re[p] += xr; im[p] += xi;
            }
        }
    }
}

void dx_init(dx_state_t *s, const dx_cfg_t *cfg)
{
    memset(s, 0, sizeof(*s));
    s->cfg = *cfg;
    for (unsigned i = 0; i < DX_FRAME; i++) {
        s->window[i] = 0.5f - 0.5f * cosf(2.0f * PI_F * (float)i / (float)(DX_FRAME - 1));
    }
    s->frames_since_speech = 67;
}

/* Peak lag (samples, parabolic) and normalised height of sum_k w_k Re(C_k e^(j 2 pi k l / 512)) */
static float peak(const float *cre, const float *cim, float norm, float *conf)
{
    float c[2 * LAGS + 1];
    for (int l = -LAGS; l <= LAGS; l++) {
        float acc = 0.0f;
        for (unsigned i = 0; i < DX_BINS; i++) {
            const float ph = 2.0f * PI_F * (float)(DX_BIN_LO + i) * (float)l / (float)(DX_FFT * INTERP);
            acc += cre[i] * cosf(ph) - cim[i] * sinf(ph);
        }
        c[l + LAGS] = acc;
    }
    int best = 0;
    for (int l = -MAX_LAG * INTERP; l <= MAX_LAG * INTERP; l++) {
        if (c[l + LAGS] > c[best + LAGS]) {
            best = l;
        }
    }
    const float ym = c[best - 1 + LAGS], y0 = c[best + LAGS], yp = c[best + 1 + LAGS];
    const float den = ym - 2.0f * y0 + yp;
    float d = 0.0f;
    if (den < 0.0f) {
        d = 0.5f * (ym - yp) / den;
        d = d > 0.5f ? 0.5f : d < -0.5f ? -0.5f : d;
    }
    *conf = norm > 0.0f ? y0 / norm : 0.0f;
    return ((float)best + d) / (float)INTERP;
}

void dx_process(dx_state_t *s, const float mics[4][DX_FRAME])
{
    /* energy on a 100 Hz high-passed copy, as doa.c */
    double e = 0.0;
    const float a = expf(-2.0f * PI_F * 100.0f / 16000.0f);
    for (unsigned m = 0; m < 4; m++) {
        for (unsigned i = 0; i < DX_FRAME; i++) {
            const float y = a * (s->hpf_y[m] + mics[m][i] - s->hpf_x[m]);
            s->hpf_x[m] = mics[m][i];
            s->hpf_y[m] = y;
            e += (double)y * y;
        }
    }
    const float energy = (float)(e / (4.0 * DX_FRAME));
    if (s->noise_floor <= 0.0f || energy < s->noise_floor) {
        s->noise_floor = energy;
    } else {
        s->noise_floor *= 1.002f;
    }
    int speech = energy > 1e-9f && energy > s->noise_floor * 4.0f;

    float re[4][DX_FFT], im[4][DX_FFT];
    for (unsigned m = 0; m < 4; m++) {
        memset(re[m], 0, sizeof(re[m]));
        memset(im[m], 0, sizeof(im[m]));
        for (unsigned i = 0; i < DX_FRAME; i++) {
            re[m][i] = mics[m][i] * s->window[i];
        }
        fft256(re[m], im[m]);
    }

    /* per bin weights */
    float w[DX_BINS];
    float p_bin[DX_BINS];
    float wsum = 0.0f;
    for (unsigned i = 0; i < DX_BINS; i++) {
        const unsigned k = DX_BIN_LO + i;
        float p = 0.0f;
        for (unsigned m = 0; m < 4; m++) {
            p += re[m][k] * re[m][k] + im[m][k] * im[m][k];
        }
        w[i] = 1.0f;
        if (s->cfg.weight == DX_W_CSD) {
            /* speech presence against the averaged noise power */
            float g = (p / (s->noise_pow[i] * 1.5f + 1e-30f) - 1.0f) / 6.0f;
            w[i] = s->noise_frames < 10 ? 1.0f : g < 0.0f ? 0.0f : g > 1.0f ? 1.0f : g;
            p_bin[i] = p;
        } else if (s->cfg.weight == DX_W_SNR) {
            float *fl = &s->bin_floor[i];
            if (*fl <= 0.0f || p < *fl) {
                *fl = p;
            } else {
                *fl *= s->cfg.snr_floor_rise;
            }
            /* speech presence: 0 at the floor, 1 at 10 dB over it */
            float g = (p / (*fl * 1.5f + 1e-30f) - 1.0f) / 6.0f;
            w[i] = g < 0.0f ? 0.0f : g > 1.0f ? 1.0f : g;
        }
        wsum += w[i];
    }
    s->weight_sum = wsum;
    if (s->cfg.weight == DX_W_SNR && s->cfg.frame_gate_snr && wsum < 0.15f * DX_BINS) {
        speech = 0;
    }

    if (s->cfg.weight == DX_W_CSD && !speech) {
        /* non-speech frame: update the noise cross spectra (~0.5 s average) */
        static const unsigned na[2] = { 0, 2 }, nb[2] = { 1, 3 };
        const float b = s->noise_frames < 33 ? 1.0f / (float)(s->noise_frames + 1) : 0.03f;
        for (unsigned i = 0; i < DX_BINS; i++) {
            const unsigned k = DX_BIN_LO + i;
            for (unsigned q = 0; q < 2; q++) {
                const float ar = re[na[q]][k], ai = im[na[q]][k], br = re[nb[q]][k], bi = im[nb[q]][k];
                s->noise_re[q][i] += b * ((br * ar + bi * ai) - s->noise_re[q][i]);
                s->noise_im[q][i] += b * ((bi * ar - br * ai) - s->noise_im[q][i]);
            }
            s->noise_pow[i] += b * (p_bin[i] - s->noise_pow[i]);
        }
        s->noise_frames++;
    }
    s->updated = 0;
    if (!speech) {
        if (s->frames_since_speech < 67) {
            s->frames_since_speech++;
        }
        return;
    }
    const float alpha = s->frames_since_speech >= 67 ? 0.0f : s->cfg.smoothing;
    s->frames_since_speech = 0;

    static const unsigned pa[2] = { 0, 2 }, pb[2] = { 1, 3 };   /* N-S, E-W: peaks at t_b - t_a */
    float norm[2] = { 0.0f, 0.0f };
    for (unsigned q = 0; q < 2; q++) {
        for (unsigned i = 0; i < DX_BINS; i++) {
            const unsigned k = DX_BIN_LO + i;
            const float ar = re[pa[q]][k], ai = im[pa[q]][k], br = re[pb[q]][k], bi = im[pb[q]][k];
            float gr = br * ar + bi * ai;
            float gi = bi * ar - br * ai;
            if (s->cfg.weight == DX_W_CSD) {
                gr -= s->noise_re[q][i];
                gi -= s->noise_im[q][i];
            }
            const float mag = sqrtf(gr * gr + gi * gi);
            float scale = 0.0f;
            if (mag > 1e-30f) {
                if (s->cfg.weight == DX_W_BETA) {
                    scale = powf(mag, -s->cfg.beta);
                } else {
                    scale = w[i] / mag;
                }
            }
            gr *= scale;
            gi *= scale;
            float *cr = &s->cross_re[q][i], *ci = &s->cross_im[q][i];
            *cr = alpha * *cr + (1.0f - alpha) * gr;
            *ci = alpha * *ci + (1.0f - alpha) * gi;
            norm[q] += sqrtf(*cr * *cr + *ci * *ci);
        }
    }
    float cns, cew;
    const float tns = peak(s->cross_re[0], s->cross_im[0], norm[0], &cns);
    const float tew = peak(s->cross_re[1], s->cross_im[1], norm[1], &cew);
    const float conf = cns < cew ? cns : cew;
    const float tsq = tns * tns + tew * tew;
    if (conf < 0.3f || tsq < 0.0625f || tsq > (float)(MAX_LAG * MAX_LAG)) {
        return;
    }
    float az = atan2f(tew, tns) * 180.0f / PI_F;
    if (az < 0.0f) {
        az += 360.0f;
    }
    s->azimuth_deg = (unsigned)(az + 0.5f) % 360;
    s->confidence = conf;
    s->updated = 1;
}
