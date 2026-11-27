/* Superdirective beamformer prototype, see sim_sdbeam.h */
#include "sim_sdbeam.h"

#include <complex.h>
#include <math.h>
#include <string.h>

#define PI_D 3.14159265358979323846
#define FS 16000.0
#define RING_RADIUS 0.0355
#define SPEED_OF_SOUND 343.0

static const double mic_az_deg[SDB_MICS] = { 0.0, 180.0, 90.0, 270.0 };   /* N, S, E, W */

static float window_[SDB_WIN];
static int window_ready;

static void init_window(void)
{
    if (window_ready) {
        return;
    }
    /* Periodic Hann, square root: analysis * synthesis sums to 1 at 50% overlap */
    for (unsigned i = 0; i < SDB_WIN; i++) {
        window_[i] = (float)sqrt(0.5 - 0.5 * cos(2.0 * PI_D * i / SDB_WIN));
    }
    window_ready = 1;
}

static void fft(double complex *a, unsigned n, int inverse)
{
    for (unsigned i = 1, j = 0; i < n; i++) {
        unsigned bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            const double complex t = a[i];
            a[i] = a[j];
            a[j] = t;
        }
    }
    for (unsigned len = 2; len <= n; len <<= 1) {
        const double ang = 2.0 * PI_D / len * (inverse ? 1.0 : -1.0);
        const double complex wl = cos(ang) + I * sin(ang);
        for (unsigned i = 0; i < n; i += len) {
            double complex w = 1.0;
            for (unsigned j = 0; j < len / 2; j++) {
                const double complex u = a[i + j];
                const double complex v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wl;
            }
        }
    }
    if (inverse) {
        for (unsigned i = 0; i < n; i++) {
            a[i] /= n;
        }
    }
}

/* Solves A x = b for a 4x4 Hermitian positive definite A (Gaussian elimination, partial pivoting) */
static void solve4(double complex A[SDB_MICS][SDB_MICS], double complex b[SDB_MICS])
{
    for (int c = 0; c < SDB_MICS; c++) {
        int p = c;
        for (int r = c + 1; r < SDB_MICS; r++) {
            if (cabs(A[r][c]) > cabs(A[p][c])) {
                p = r;
            }
        }
        for (int k = 0; k < SDB_MICS; k++) {
            const double complex t = A[c][k];
            A[c][k] = A[p][k];
            A[p][k] = t;
        }
        const double complex tb = b[c];
        b[c] = b[p];
        b[p] = tb;
        for (int r = c + 1; r < SDB_MICS; r++) {
            const double complex f = A[r][c] / A[c][c];
            for (int k = c; k < SDB_MICS; k++) {
                A[r][k] -= f * A[c][k];
            }
            b[r] -= f * b[c];
        }
    }
    for (int c = SDB_MICS - 1; c >= 0; c--) {
        for (int k = c + 1; k < SDB_MICS; k++) {
            b[c] -= A[c][k] * b[k];
        }
        b[c] /= A[c][c];
    }
}

static void steering_vector(double freq, double az_deg, double complex d[SDB_MICS])
{
    const double az = az_deg * PI_D / 180.0;
    for (int m = 0; m < SDB_MICS; m++) {
        const double ma = mic_az_deg[m] * PI_D / 180.0;
        /* plane wave from az: a mic towards the source hears it earlier */
        const double tau = -RING_RADIUS * cos(ma - az) / SPEED_OF_SOUND;
        d[m] = cexp(-I * 2.0 * PI_D * freq * tau);
    }
}

static void diffuse_coherence(double freq, double G[SDB_MICS][SDB_MICS])
{
    for (int m = 0; m < SDB_MICS; m++) {
        for (int n = 0; n < SDB_MICS; n++) {
            const double am = mic_az_deg[m] * PI_D / 180.0;
            const double an = mic_az_deg[n] * PI_D / 180.0;
            const double dx = RING_RADIUS * (cos(am) - cos(an));
            const double dy = RING_RADIUS * (sin(am) - sin(an));
            const double kd = 2.0 * PI_D * freq / SPEED_OF_SOUND * sqrt(dx * dx + dy * dy);
            G[m][n] = kd < 1e-9 ? 1.0 : sin(kd) / kd;
        }
    }
}

/* MVDR weights against diffuse noise with loading mu; returns the white noise gain */
static double mvdr(double freq, const double complex d[SDB_MICS], double mu, double complex w[SDB_MICS])
{
    double G[SDB_MICS][SDB_MICS];
    diffuse_coherence(freq, G);
    double complex A[SDB_MICS][SDB_MICS];
    for (int m = 0; m < SDB_MICS; m++) {
        for (int n = 0; n < SDB_MICS; n++) {
            A[m][n] = G[m][n] + (m == n ? mu : 0.0);
        }
        w[m] = d[m];
    }
    solve4(A, w);
    double complex dhw = 0.0;
    for (int m = 0; m < SDB_MICS; m++) {
        dhw += conj(d[m]) * w[m];
    }
    double ww = 0.0;
    for (int m = 0; m < SDB_MICS; m++) {
        w[m] /= dhw;
        ww += creal(w[m] * conj(w[m]));
    }
    return 1.0 / ww;
}

static void compute_weights(sdb_weights_t *wt, unsigned az_deg, float wng_min_db, float *min_wng_db)
{
    const double wng_min = pow(10.0, wng_min_db / 10.0);
    double lowest = 1e9;
    for (unsigned k = 0; k < SDB_BINS; k++) {
        const double f = (double)k * FS / SDB_FFT;
        double complex d[SDB_MICS];
        double complex w[SDB_MICS];
        steering_vector(f, az_deg, d);
        /* white noise gain rises with the loading: bisect on log10(mu) for the smallest that meets the floor */
        double wng = mvdr(f, d, 1e-9, w);
        if (wng < wng_min) {
            double lo = -9.0;
            double hi = 3.0;
            for (int it = 0; it < 40; it++) {
                const double mid = 0.5 * (lo + hi);
                if (mvdr(f, d, pow(10.0, mid), w) >= wng_min) {
                    hi = mid;
                } else {
                    lo = mid;
                }
            }
            wng = mvdr(f, d, pow(10.0, hi), w);
        }
        if (wng < lowest) {
            lowest = wng;
        }
        for (int m = 0; m < SDB_MICS; m++) {
            /* output = sum_m conj(w_m) X_m */
            wt->re[m][k] = (float)creal(conj(w[m]));
            wt->im[m][k] = (float)cimag(conj(w[m]));
            wt->align_re[m][k] = (float)creal(conj(d[m]));
            wt->align_im[m][k] = (float)cimag(conj(d[m]));
        }
    }
    for (int m = 0; m < SDB_MICS; m++) {
        wt->block[m] = (float)(cos((mic_az_deg[m] - az_deg) * PI_D / 180.0) / 2.0);
    }
    if (min_wng_db) {
        *min_wng_db = (float)(10.0 * log10(lowest));
    }
}

void sdb_init(sdb_t *s, float wng_min_db, unsigned azimuth_deg)
{
    init_window();
    memset(s, 0, sizeof(*s));
    s->wng_min_db = wng_min_db;
    s->azimuth_deg = azimuth_deg % 360;
    compute_weights(&s->w, s->azimuth_deg, wng_min_db, &s->min_wng_db);
    for (unsigned b = 0; b < SDB_MULTI; b++) {
        compute_weights(&s->multi[b], b * 360 / SDB_MULTI, wng_min_db, NULL);
    }
}

void sdb_steer(sdb_t *s, unsigned azimuth_deg)
{
    azimuth_deg %= 360;
    if (azimuth_deg == s->azimuth_deg) {
        return;
    }
    s->w_prev = s->w;
    s->fade = 1;
    s->azimuth_deg = azimuth_deg;
    compute_weights(&s->w, azimuth_deg, s->wng_min_db, &s->min_wng_db);
}

void sdb_analyse(sdb_t *s, const int32_t mics[SDB_MICS][SDB_HOP])
{
    for (int m = 0; m < SDB_MICS; m++) {
        double complex buf[SDB_FFT];
        for (unsigned i = 0; i < SDB_WIN - SDB_HOP; i++) {
            buf[i] = s->in_hist[m][i] * window_[i];
        }
        for (unsigned i = 0; i < SDB_HOP; i++) {
            const float x = (float)mics[m][i] / 2147483648.0f;
            buf[SDB_WIN - SDB_HOP + i] = x * window_[SDB_WIN - SDB_HOP + i];
            s->in_hist[m][i] = x;
        }
        for (unsigned i = SDB_WIN; i < SDB_FFT; i++) {
            buf[i] = 0.0;
        }
        fft(buf, SDB_FFT, 0);
        for (unsigned k = 0; k < SDB_BINS; k++) {
            s->spec_re[m][k] = (float)creal(buf[k]);
            s->spec_im[m][k] = (float)cimag(buf[k]);
        }
    }
}

static void apply(const sdb_t *s, const sdb_weights_t *w, double complex Y[2][SDB_BINS])
{
    for (unsigned k = 0; k < SDB_BINS; k++) {
        double complex t = 0.0;
        double complex b = 0.0;
        for (int m = 0; m < SDB_MICS; m++) {
            const double complex x = s->spec_re[m][k] + I * s->spec_im[m][k];
            t += (w->re[m][k] + I * w->im[m][k]) * x;
            b += w->block[m] * (w->align_re[m][k] + I * w->align_im[m][k]) * x;
        }
        Y[0][k] = t;
        Y[1][k] = b;
    }
}

static int32_t to_q31(double v)
{
    if (v > 0.9999999) {
        return INT32_MAX;
    }
    if (v < -0.9999999) {
        return -INT32_MAX;
    }
    return (int32_t)lrint(v * 2147483648.0);
}

void sdb_beams(sdb_t *s, int32_t out[2][SDB_HOP])
{
    double complex Y[2][SDB_BINS];
    apply(s, &s->w, Y);
    if (s->fade) {
        /* the previous weights fade out over this frame: average the two spectra */
        double complex P[2][SDB_BINS];
        apply(s, &s->w_prev, P);
        for (int c = 0; c < 2; c++) {
            for (unsigned k = 0; k < SDB_BINS; k++) {
                Y[c][k] = 0.5 * (Y[c][k] + P[c][k]);
            }
        }
        s->fade = 0;
    }
    for (int c = 0; c < 2; c++) {
        double complex buf[SDB_FFT];
        for (unsigned k = 0; k < SDB_BINS; k++) {
            buf[k] = Y[c][k];
        }
        for (unsigned k = SDB_BINS; k < SDB_FFT; k++) {
            buf[k] = conj(Y[c][SDB_FFT - k]);
        }
        fft(buf, SDB_FFT, 1);
        /* overlap-add with the synthesis window; emit the completed first hop */
        for (unsigned i = 0; i < SDB_HOP; i++) {
            out[c][i] = to_q31(s->ola[c][i] + creal(buf[i]) * window_[i]);
        }
        for (unsigned i = 0; i < SDB_WIN - SDB_HOP; i++) {
            s->ola[c][i] = (float)(creal(buf[SDB_HOP + i]) * window_[SDB_HOP + i]);
        }
    }
}

void sdb_multi_power(const sdb_t *s, float power[SDB_MULTI])
{
    const unsigned k0 = 300 * SDB_FFT / 16000;
    const unsigned k1 = 3400 * SDB_FFT / 16000;
    for (unsigned b = 0; b < SDB_MULTI; b++) {
        const sdb_weights_t *w = &s->multi[b];
        double p = 0.0;
        for (unsigned k = k0; k <= k1; k++) {
            double complex t = 0.0;
            for (int m = 0; m < SDB_MICS; m++) {
                t += (w->re[m][k] + I * w->im[m][k]) * (s->spec_re[m][k] + I * s->spec_im[m][k]);
            }
            p += creal(t * conj(t));
        }
        power[b] = (float)p;
    }
}

float sdb_directivity_db(const sdb_t *s, float freq_hz)
{
    const unsigned k = (unsigned)lrintf(freq_hz * SDB_FFT / 16000.0f);
    double G[SDB_MICS][SDB_MICS];
    diffuse_coherence((double)k * FS / SDB_FFT, G);
    double complex w[SDB_MICS];
    for (int m = 0; m < SDB_MICS; m++) {
        w[m] = s->w.re[m][k] - I * s->w.im[m][k];  /* back from w* */
    }
    double num = 0.0;
    double complex d[SDB_MICS];
    steering_vector((double)k * FS / SDB_FFT, s->azimuth_deg, d);
    double complex wd = 0.0;
    for (int m = 0; m < SDB_MICS; m++) {
        wd += conj(w[m]) * d[m];
    }
    num = creal(wd * conj(wd));
    double complex den = 0.0;
    for (int m = 0; m < SDB_MICS; m++) {
        for (int n = 0; n < SDB_MICS; n++) {
            den += conj(w[m]) * G[m][n] * w[n];
        }
    }
    return (float)(10.0 * log10(num / creal(den)));
}
