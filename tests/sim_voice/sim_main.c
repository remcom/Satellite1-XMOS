/*
 * Voice pipeline simulation: renders rooms with a talker, a TV, music on the
 * device speaker and noise, runs them through the pipeline replica
 * (sim_pipeline.c) as the firmware is configured before and after each
 * change, and prints one JSON object per measurement.
 *
 * usage: sim_voice <speech dir> <scenario|all> [csv dir]
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim_pipeline.h"
#include "sim_room.h"

#define PI 3.14159265358979
#define FRAME SIM_FRAME
#define DB(x) (10.0 * log10((x) + 1e-30))

static const char *speech_dir;
static const char *csv_dir;

/* ---------------------------------------------------------------- FFT convolution */

typedef struct { double re, im; } cpx_t;

static void fft(cpx_t *a, unsigned n, int inverse)
{
    for (unsigned i = 1, j = 0; i < n; i++) {
        unsigned bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            cpx_t t = a[i]; a[i] = a[j]; a[j] = t;
        }
    }
    for (unsigned len = 2; len <= n; len <<= 1) {
        const double ang = 2.0 * PI / (double)len * (inverse ? 1.0 : -1.0);
        const cpx_t wl = { cos(ang), sin(ang) };
        for (unsigned i = 0; i < n; i += len) {
            cpx_t w = { 1.0, 0.0 };
            for (unsigned k = 0; k < len / 2; k++) {
                const cpx_t u = a[i + k];
                const cpx_t v = { a[i + k + len / 2].re * w.re - a[i + k + len / 2].im * w.im,
                                  a[i + k + len / 2].re * w.im + a[i + k + len / 2].im * w.re };
                a[i + k] = (cpx_t){ u.re + v.re, u.im + v.im };
                a[i + k + len / 2] = (cpx_t){ u.re - v.re, u.im - v.im };
                const double wr = w.re * wl.re - w.im * wl.im;
                w.im = w.re * wl.im + w.im * wl.re;
                w.re = wr;
            }
        }
    }
    if (inverse) {
        for (unsigned i = 0; i < n; i++) {
            a[i].re /= n;
            a[i].im /= n;
        }
    }
}

/* out += src * rir, overlap-add */
static void convolve_add(const asig_t *src, const float *rir, unsigned rir_len, asig_t *out)
{
    const unsigned n = 16384;
    const unsigned block = n - rir_len;
    cpx_t *h = calloc(n, sizeof(cpx_t));
    cpx_t *x = malloc(n * sizeof(cpx_t));
    for (unsigned i = 0; i < rir_len; i++) {
        h[i].re = rir[i];
    }
    fft(h, n, 0);
    for (size_t start = 0; start < src->n; start += block) {
        memset(x, 0, n * sizeof(cpx_t));
        for (unsigned i = 0; i < block && start + i < src->n; i++) {
            x[i].re = src->x[start + i];
        }
        fft(x, n, 0);
        for (unsigned i = 0; i < n; i++) {
            const double re = x[i].re * h[i].re - x[i].im * h[i].im;
            x[i].im = x[i].re * h[i].im + x[i].im * h[i].re;
            x[i].re = re;
        }
        fft(x, n, 1);
        for (unsigned i = 0; i < n && start + i < out->n; i++) {
            out->x[start + i] += (float)x[i].re;
        }
    }
    free(h);
    free(x);
}

static void render(const room_t *room, const float pos[3], const asig_t *src, asig_t out[SIM_MICS])
{
    float *rir = malloc(SIM_RIR_LEN * sizeof(float));
    for (unsigned m = 0; m < SIM_MICS; m++) {
        room_rir(room, pos, m, rir);
        convolve_add(src, rir, SIM_RIR_LEN, &out[m]);
    }
    free(rir);
}

/* ---------------------------------------------------------------- scene */

#define MAX_UTT 16

typedef struct {
    float start_s;
    int wav;            /* english_usa/<wav>.wav */
    int wake;           /* this utterance is a wake word: reported 300 ms after it ends */
} utt_t;

typedef struct {
    const char *name;
    float duration_s;
    float rt60_beta;
    /* talker */
    float talker_az, talker_dist, talker_db;    /* dBFS rms at the mics while talking */
    utt_t utt[MAX_UTT];
    int n_utt;
    /* TV: mandarin speech, continuous */
    int tv;
    float tv_az, tv_db;
    /* music on the device speaker */
    int music;
    float music_db;     /* dBFS rms of the echo at the mics */
    float music_width;
    float music_from_s, music_to_s;
    int speaker_left_only;      /* TAS2780 set to the left channel instead of the (L+R)/2 downmix */
    float speaker_drive;
    /* noise */
    float noise_db;
    float fan_az, fan_db;       /* a point noise source (fan, hood); fan_db < -200: none */
    uint32_t seed;
} scene_t;

typedef struct {
    size_t n;
    asig_t mic[SIM_MICS];       /* total */
    asig_t talker[SIM_MICS];    /* talker component */
    asig_t speech[SIM_MICS];    /* talker + TV, for the VNR oracle */
    asig_t echo[SIM_MICS];
    asig_t ref_l, ref_r;        /* speaker reference */
    int *wake_frame;            /* per frame: wake reported */
    int *conv_frame;            /* per frame: conversation active */
    size_t utt_start[MAX_UTT], utt_len[MAX_UTT];
} rendered_t;

static asig_t load_english(int idx)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/english_usa/%d.wav", speech_dir, idx);
    return sig_load_wav(path);
}

static float span_rms(const asig_t *s, const rendered_t *r, int n_utt)
{
    double e = 0.0;
    size_t cnt = 0;
    for (int u = 0; u < n_utt; u++) {
        for (size_t i = r->utt_start[u]; i < r->utt_start[u] + r->utt_len[u] && i < s->n; i++) {
            e += (double)s->x[i] * s->x[i];
            cnt++;
        }
    }
    return cnt ? (float)sqrt(e / cnt) : 0.0f;
}

static void scale_mics(asig_t m[SIM_MICS], float g)
{
    for (unsigned c = 0; c < SIM_MICS; c++) {
        for (size_t i = 0; i < m[c].n; i++) {
            m[c].x[i] *= g;
        }
    }
}

static float mics_rms(asig_t m[SIM_MICS], size_t a, size_t b)
{
    double e = 0.0;
    for (unsigned c = 0; c < SIM_MICS; c++) {
        for (size_t i = a; i < b && i < m[c].n; i++) {
            e += (double)m[c].x[i] * m[c].x[i];
        }
    }
    return (float)sqrt(e / (SIM_MICS * (double)(b - a)));
}

static rendered_t render_scene(const scene_t *sc)
{
    rendered_t r;
    memset(&r, 0, sizeof(r));
    r.n = (size_t)(sc->duration_s * SIM_FS) / FRAME * FRAME;
    const room_t room = { { 6.0f, 5.0f, 2.7f }, { 3.1f, 2.6f, 0.85f }, sc->rt60_beta };
    for (unsigned m = 0; m < SIM_MICS; m++) {
        r.mic[m] = sig_alloc(r.n);
        r.talker[m] = sig_alloc(r.n);
        r.speech[m] = sig_alloc(r.n);
        r.echo[m] = sig_alloc(r.n);
    }
    r.ref_l = sig_alloc(r.n);
    r.ref_r = sig_alloc(r.n);
    r.wake_frame = calloc(r.n / FRAME, sizeof(int));
    r.conv_frame = calloc(r.n / FRAME, sizeof(int));

    /* talker */
    if (sc->n_utt) {
        asig_t dry = sig_alloc(r.n);
        for (int u = 0; u < sc->n_utt; u++) {
            asig_t w = load_english(sc->utt[u].wav);
            sig_scale_to_rms(&w, 0.1f);
            r.utt_start[u] = (size_t)(sc->utt[u].start_s * SIM_FS);
            r.utt_len[u] = w.n;
            sig_add(&dry, &w, r.utt_start[u], 1.0f);
            if (sc->utt[u].wake) {
                const size_t f = (r.utt_start[u] + w.n) / FRAME + (size_t)(0.3f * SIM_FS / FRAME);
                if (f < r.n / FRAME) {
                    r.wake_frame[f] = 1;
                }
                /* conversation until 1.5 s after the last utterance of this request */
                size_t end = r.utt_start[u] + w.n;
                for (int v = u + 1; v < sc->n_utt && !sc->utt[v].wake; v++) {
                    end = (size_t)(sc->utt[v].start_s * SIM_FS) + 16000;
                }
                for (size_t k = f; k < (end + (size_t)(1.5f * SIM_FS)) / FRAME && k < r.n / FRAME; k++) {
                    r.conv_frame[k] = 1;
                }
            }
            sig_free(&w);
        }
        float pos[3];
        room_source_pos(sc->talker_az, sc->talker_dist, 0.6f, pos);
        render(&room, pos, &dry, r.talker);
        /* level while talking */
        float e = 0.0f;
        for (unsigned m = 0; m < SIM_MICS; m++) {
            const float v = span_rms(&r.talker[m], &r, sc->n_utt);
            e += v * v;
        }
        scale_mics(r.talker, powf(10.0f, sc->talker_db / 20.0f) / sqrtf(e / SIM_MICS));
        sig_free(&dry);
    }

    /* TV: mandarin numbers back to back */
    asig_t tv_m[SIM_MICS];
    for (unsigned m = 0; m < SIM_MICS; m++) {
        tv_m[m] = sig_alloc(r.n);
    }
    if (sc->tv) {
        asig_t dry = sig_alloc(r.n);
        size_t at = 0;
        static const int order[] = { 3, 5, 7, 9, 11, 13, 15, 17, 4, 6, 8, 10, 12, 14, 16, 18 };
        for (unsigned k = 0; at < r.n; k++) {
            char path[512];
            snprintf(path, sizeof(path), "%s/mandarin_mainland/%d.wav", speech_dir, order[k % 16]);
            asig_t w = sig_load_wav(path);
            sig_scale_to_rms(&w, 0.1f);
            sig_add(&dry, &w, at, 1.0f);
            at += w.n + SIM_FS / 8;
            sig_free(&w);
        }
        float pos[3];
        room_source_pos(sc->tv_az, 2.2f, 0.3f, pos);
        render(&room, pos, &dry, tv_m);
        scale_mics(tv_m, powf(10.0f, sc->tv_db / 20.0f) / mics_rms(tv_m, 0, r.n));
        sig_free(&dry);
    }

    /* music on the speaker, just below the ring centre */
    if (sc->music) {
        asig_t l = sig_alloc(r.n);
        asig_t rr = sig_alloc(r.n);
        music_synth(&l, &rr, 3.0f, sc->music_width, sc->seed * 7 + 1);
        const size_t a = (size_t)(sc->music_from_s * SIM_FS);
        const size_t b = (size_t)(sc->music_to_s * SIM_FS);
        for (size_t i = 0; i < r.n; i++) {
            if (i < a || i >= b) {
                l.x[i] = 0.0f;
                rr.x[i] = 0.0f;
            }
        }
        /* reference level: -18 dBFS rms */
        const float g = 0.125f / fmaxf(sig_rms(&l, a, b - a), sig_rms(&rr, a, b - a));
        for (size_t i = 0; i < r.n; i++) {
            l.x[i] *= g;
            rr.x[i] *= g;
        }
        asig_t spk = sig_alloc(r.n);
        for (size_t i = 0; i < r.n; i++) {
            spk.x[i] = sc->speaker_left_only ? l.x[i] : 0.5f * (l.x[i] + rr.x[i]);
        }
        speaker_nonlinearity(&spk, sc->speaker_drive);
        const float pos[3] = { 0.0f, 0.0f, -0.04f };
        render(&room, pos, &spk, r.echo);
        scale_mics(r.echo, powf(10.0f, sc->music_db / 20.0f) / mics_rms(r.echo, a, b));
        r.ref_l = l;
        r.ref_r = rr;
        sig_free(&spk);
    }

    /* noise: three far point sources, plus an optional fan */
    asig_t noise_m[SIM_MICS];
    for (unsigned m = 0; m < SIM_MICS; m++) {
        noise_m[m] = sig_alloc(r.n);
    }
    static const float naz[3] = { 45.0f, 160.0f, 280.0f };
    for (int k = 0; k < 3; k++) {
        asig_t nz = sig_alloc(r.n);
        noise_synth(&nz, sc->seed * 31 + 3 + k);
        float pos[3];
        room_source_pos(naz[k], 2.2f, 1.2f - 0.5f * k, pos);
        render(&room, pos, &nz, noise_m);
        sig_free(&nz);
    }
    scale_mics(noise_m, powf(10.0f, sc->noise_db / 20.0f) / mics_rms(noise_m, 0, r.n));
    if (sc->fan_db > -200.0f) {
        asig_t fan_m[SIM_MICS];
        for (unsigned m = 0; m < SIM_MICS; m++) {
            fan_m[m] = sig_alloc(r.n);
        }
        asig_t nz = sig_alloc(r.n);
        noise_synth(&nz, sc->seed * 17 + 11);
        /* a hum on top of the broadband noise */
        for (size_t i = 0; i < r.n; i++) {
            nz.x[i] += 0.3f * sinf(2.0f * (float)PI * 120.0f * (float)i / SIM_FS);
        }
        float pos[3];
        room_source_pos(sc->fan_az, 1.8f, 0.4f, pos);
        render(&room, pos, &nz, fan_m);
        scale_mics(fan_m, powf(10.0f, sc->fan_db / 20.0f) / mics_rms(fan_m, 0, r.n));
        for (unsigned m = 0; m < SIM_MICS; m++) {
            sig_add(&noise_m[m], &fan_m[m], 0, 1.0f);
            sig_free(&fan_m[m]);
        }
        sig_free(&nz);
    }

    /* self noise ~-90 dBFS, then the sum */
    uint32_t rs = sc->seed * 101 + 5;
    for (unsigned m = 0; m < SIM_MICS; m++) {
        for (size_t i = 0; i < r.n; i++) {
            r.speech[m].x[i] = r.talker[m].x[i] + tv_m[m].x[i];
            r.mic[m].x[i] = r.talker[m].x[i] + tv_m[m].x[i] + r.echo[m].x[i] + noise_m[m].x[i] +
                            3e-5f * sim_rand(&rs);
        }
        sig_free(&tv_m[m]);
        sig_free(&noise_m[m]);
    }
    return r;
}

static void free_rendered(rendered_t *r)
{
    for (unsigned m = 0; m < SIM_MICS; m++) {
        sig_free(&r->mic[m]);
        sig_free(&r->talker[m]);
        sig_free(&r->speech[m]);
        sig_free(&r->echo[m]);
    }
    sig_free(&r->ref_l);
    sig_free(&r->ref_r);
    free(r->wake_frame);
    free(r->conv_frame);
}

/* ---------------------------------------------------------------- run */

typedef struct {
    size_t frames;
    asig_t ch0, ch1;
    float *erle_in, *erle_out;  /* per frame AEC energies */
    doa_result_t *doa;
    uint16_t *beam;
    uint8_t *ch0_mic;
    int *doa_gated;
} run_t;

static int32_t to_q31(float v)
{
    if (v > 0.9999999f) {
        return INT32_MAX;
    }
    if (v < -0.9999999f) {
        return -INT32_MAX;
    }
    return (int32_t)lrintf(v * 2147483648.0f);
}

/*
 * talker_sign -1 runs the scene with the talker's polarity inverted, for the
 * phase inversion (Hagerman) separation, see snr_db().
 */
static run_t run_pipeline_sign(const rendered_t *r, const sim_cfg_t *cfg, int talker_sign)
{
    run_t o;
    o.frames = r->n / FRAME;
    o.ch0 = sig_alloc(r->n);
    o.ch1 = sig_alloc(r->n);
    o.erle_in = calloc(o.frames, sizeof(float));
    o.erle_out = calloc(o.frames, sizeof(float));
    o.doa = calloc(o.frames, sizeof(doa_result_t));
    o.beam = calloc(o.frames, sizeof(uint16_t));
    o.ch0_mic = calloc(o.frames, sizeof(uint8_t));
    o.doa_gated = calloc(o.frames, sizeof(int));
    sim_pipeline_init(cfg);

    float vnr_lag[3] = { 0.05f, 0.05f, 0.05f };
    for (size_t f = 0; f < o.frames; f++) {
        int32_t raw[4][FRAME];
        int32_t rl[FRAME];
        int32_t rr[FRAME];
        double es = 0.0;
        double en = 0.0;
        for (unsigned i = 0; i < FRAME; i++) {
            const size_t k = f * FRAME + i;
            for (unsigned m = 0; m < 4; m++) {
                raw[m][i] = to_q31(r->mic[m].x[k] - (talker_sign < 0 ? 2.0f * r->talker[m].x[k] : 0.0f));
            }
            rl[i] = to_q31(r->ref_l.n ? r->ref_l.x[k] : 0.0f);
            rr[i] = to_q31(r->ref_r.n ? r->ref_r.x[k] : 0.0f);
            const float s = r->speech[0].x[k];
            /* what reaches the IC besides speech: noise and ~25 dB of residual echo */
            const float n = r->mic[0].x[k] - s - r->echo[0].x[k] + 0.056f * r->echo[0].x[k];
            es += (double)s * s;
            en += (double)n * n;
        }
        /* VNR oracle: speech-to-noise of the frame, two frames of inference latency */
        const double snr_db = DB(es) - DB(en);
        const float vnr_now = 0.02f + 0.93f / (1.0f + expf(-(float)(snr_db - 3.0) / 2.0f));
        const float vnr = vnr_lag[0];
        vnr_lag[0] = vnr_lag[1];
        vnr_lag[1] = vnr_now;

        sim_out_t out;
        sim_pipeline_frame((const int32_t (*)[FRAME])raw, rl, rr, vnr,
                           r->wake_frame[f], r->conv_frame[f], &out);
        for (unsigned i = 0; i < FRAME; i++) {
            o.ch0.x[f * FRAME + i] = (float)out.ch0[i] / 2147483648.0f;
            o.ch1.x[f * FRAME + i] = (float)out.ch1[i] / 2147483648.0f;
        }
        o.erle_in[f] = out.aec_in_energy;
        o.erle_out[f] = out.aec_out_energy;
        o.doa[f] = out.doa;
        o.beam[f] = out.beam_azimuth;
        o.ch0_mic[f] = out.ch0_mic;
        o.doa_gated[f] = out.doa_gated;
    }
    return o;
}

static run_t run_pipeline(const rendered_t *r, const sim_cfg_t *cfg)
{
    return run_pipeline_sign(r, cfg, 1);
}

static void free_run(run_t *o)
{
    sig_free(&o->ch0);
    sig_free(&o->ch1);
    free(o->erle_in);
    free(o->erle_out);
    free(o->doa);
    free(o->beam);
    free(o->ch0_mic);
    free(o->doa_gated);
}

/* ---------------------------------------------------------------- metrics */

/*
 * Output SNR of the talker by phase inversion (Hagerman & Olofsson 2004): the
 * scene runs once as is (a) and once with the talker inverted (b). The talker
 * at the output is (a - b) / 2, everything else (noise, echo, TV) is
 * (a + b) / 2, both through exactly the processing the mix got, so linear
 * filtering of the talker (the IC filter, the beam) is not counted against
 * it. Valid while the processing does not depend on the talker's polarity:
 * the AEC barely adapts in double talk and the IC is held during speech.
 * Over [a, b) plus 100 ms for the pipeline delay (the IC delays by 37.5 ms).
 */
static double snr_db(const asig_t *pa, const asig_t *pb, size_t a, size_t b)
{
    b += SIM_FS / 10;
    double et = 0.0;
    double en = 0.0;
    for (size_t i = a; i < b && i < pa->n; i++) {
        const double t = 0.5 * ((double)pa->x[i] - pb->x[i]);
        const double n = 0.5 * ((double)pa->x[i] + pb->x[i]);
        et += t * t;
        en += n * n;
    }
    return DB(et) - DB(en);
}

typedef struct {
    run_t a;
    run_t b;
} run_pair_t;

static run_pair_t run_pair(const rendered_t *r, const sim_cfg_t *cfg)
{
    run_pair_t p = { run_pipeline_sign(r, cfg, 1), run_pipeline_sign(r, cfg, -1) };
    return p;
}

static void free_pair(run_pair_t *p)
{
    free_run(&p->a);
    free_run(&p->b);
}

static double erle_db(const run_t *o, size_t f0, size_t f1)
{
    double ei = 0.0;
    double eo = 0.0;
    for (size_t f = f0; f < f1 && f < o->frames; f++) {
        ei += o->erle_in[f];
        eo += o->erle_out[f];
    }
    return DB(ei) - DB(eo);
}

static double level_db(const asig_t *s, size_t a, size_t b)
{
    double e = 0.0;
    for (size_t i = a; i < b && i < s->n; i++) {
        e += (double)s->x[i] * s->x[i];
    }
    return DB(e / (double)(b - a));
}

static int cmp_double(const void *a, const void *b)
{
    const double x = *(const double *)a;
    const double y = *(const double *)b;
    return (x > y) - (x < y);
}

/* ERLE over 0.5 s windows from t0_s: mean and the 10th percentile (the dips) */
static void erle_stats(const run_t *o, float t0_s, float t1_s, double *mean, double *p10)
{
    const size_t w = 33;
    double v[512];
    int n = 0;
    double ei = 0.0;
    double eo = 0.0;
    for (size_t f = (size_t)(t0_s * SIM_FS / FRAME); f + w <= (size_t)(t1_s * SIM_FS / FRAME) && n < 512; f += w) {
        v[n++] = erle_db(o, f, f + w);
        for (size_t k = f; k < f + w; k++) {
            ei += o->erle_in[k];
            eo += o->erle_out[k];
        }
    }
    qsort(v, n, sizeof(double), cmp_double);
    *mean = DB(ei) - DB(eo);
    *p10 = n ? v[n / 10] : 0.0;
}

static void write_csv(const char *name, const run_t *runs[], const char *labels[], int nruns)
{
    if (!csv_dir) {
        return;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.csv", csv_dir, name);
    FILE *f = fopen(path, "w");
    if (!f) {
        return;
    }
    fprintf(f, "t_s");
    for (int k = 0; k < nruns; k++) {
        fprintf(f, ",%s_erle_db", labels[k]);
    }
    fprintf(f, "\n");
    const size_t w = 16;
    for (size_t fr = 0; fr + w <= runs[0]->frames; fr += w) {
        fprintf(f, "%.3f", (double)(fr + w / 2) * FRAME / SIM_FS);
        for (int k = 0; k < nruns; k++) {
            fprintf(f, ",%.2f", erle_db(runs[k], fr, fr + w));
        }
        fprintf(f, "\n");
    }
    fclose(f);
}

static unsigned ang_err(unsigned a, unsigned b)
{
    const unsigned d = (a > b ? a - b : b - a) % 360;
    return d > 180 ? 360 - d : d;
}

static sim_cfg_t base_cfg(void)
{
    sim_cfg_t c;
    memset(&c, 0, sizeof(c));
    c.aec_ref_channels = 2;
    c.ref_mix = REF_MIX_DOWNMIX;
    c.ic_fast_hold = 1;
    c.mode = MIC_MODE_PAIR_EW;
    c.policy = STEER_POLICY_WAKE_WORD;
    c.ch1_tap = 0;
    c.mic_gain_db = 20;
    c.hpf = 1;
    return c;
}

static scene_t base_scene(const char *name, float dur)
{
    scene_t s;
    memset(&s, 0, sizeof(s));
    s.name = name;
    s.duration_s = dur;
    s.rt60_beta = 0.85f;
    s.talker_az = 60.0f;
    s.talker_dist = 2.0f;
    s.talker_db = -50.0f;
    s.music_db = -30.0f;
    s.music_width = 1.0f;
    s.noise_db = -68.0f;
    s.fan_db = -1000.0f;
    s.speaker_drive = 1.5f;
    s.seed = 1;
    return s;
}

/* ---------------------------------------------------------------- scenarios */

/* 1: stereo music, mono speaker. AEC with L and R references vs the mono mix. */
static void scenario_aec_ref(void)
{
    for (int seed = 1; seed <= 3; seed++) {
        scene_t sc = base_scene("aec_ref", 30.0f);
        sc.seed = seed;
        sc.music = 1;
        sc.music_from_s = 0.0f;
        sc.music_to_s = 30.0f;
        rendered_t r = render_scene(&sc);

        sim_cfg_t c22 = base_cfg();
        sim_cfg_t c21 = base_cfg();
        c21.aec_ref_channels = 1;
        sim_cfg_t c41 = base_cfg();
        c41.aec4 = 1;
        run_t o22 = run_pipeline(&r, &c22);
        run_t o21 = run_pipeline(&r, &c21);
        run_t o41 = run_pipeline(&r, &c41);
        double m22, p22, m21, p21, m41, p41;
        erle_stats(&o22, 5.0f, 30.0f, &m22, &p22);
        erle_stats(&o21, 5.0f, 30.0f, &m21, &p21);
        erle_stats(&o41, 5.0f, 30.0f, &m41, &p41);
        printf("{\"scenario\":\"aec_ref\",\"seed\":%d,"
               "\"erle_2x2_mean\":%.2f,\"erle_2x2_p10\":%.2f,"
               "\"erle_2x1_mean\":%.2f,\"erle_2x1_p10\":%.2f,"
               "\"erle_4x1_mean\":%.2f,\"erle_4x1_p10\":%.2f,"
               "\"echo_ch1_2x2_db\":%.2f,\"echo_ch1_2x1_db\":%.2f,\"echo_ch1_4x1_db\":%.2f}\n",
               seed, m22, p22, m21, p21, m41, p41,
               level_db(&o22.ch1, 5 * SIM_FS, r.n), level_db(&o21.ch1, 5 * SIM_FS, r.n),
               level_db(&o41.ch1, 5 * SIM_FS, r.n));
        if (seed == 1) {
            const run_t *runs[] = { &o22, &o21, &o41 };
            const char *labels[] = { "normal_2x2", "normal_2x1", "aec4_4x1" };
            write_csv("aec_ref", runs, labels, 3);
        }
        free_run(&o22);
        free_run(&o21);
        free_run(&o41);
        free_rendered(&r);
    }
}

/* 1b: wake words during stereo music: echo leaking into the wake word channel */
static void scenario_aec_ref_doubletalk(void)
{
    for (int seed = 1; seed <= 3; seed++) {
        scene_t sc = base_scene("aec_ref_dt", 30.0f);
        sc.seed = seed;
        sc.music = 1;
        sc.music_to_s = 30.0f;
        sc.music_db = -32.0f;
        static const int wavs[] = { 11, 12, 17, 18, 15, 16 };
        for (int u = 0; u < 6; u++) {
            sc.utt[u] = (utt_t){ 6.0f + 4.0f * u, wavs[u], 0 };
        }
        sc.n_utt = 6;
        rendered_t r = render_scene(&sc);
        sim_cfg_t c22 = base_cfg();
        sim_cfg_t c21 = base_cfg();
        c21.aec_ref_channels = 1;
        run_pair_t o22 = run_pair(&r, &c22);
        run_pair_t o21 = run_pair(&r, &c21);
        double s22 = 0.0;
        double s21 = 0.0;
        for (int u = 0; u < 6; u++) {
            const size_t a = r.utt_start[u];
            const size_t b = a + r.utt_len[u];
            s22 += snr_db(&o22.a.ch1, &o22.b.ch1, a, b) / 6.0;
            s21 += snr_db(&o21.a.ch1, &o21.b.ch1, a, b) / 6.0;
        }
        printf("{\"scenario\":\"aec_ref_doubletalk\",\"seed\":%d,\"snr_ch1_2x2\":%.2f,\"snr_ch1_2x1\":%.2f}\n",
               seed, s22, s21);
        free_pair(&o22);
        free_pair(&o21);
        free_rendered(&r);
    }
}

/* 2: TAS2780 set to the left channel: downmix reference vs a matching one */
static void scenario_ref_mix(void)
{
    for (int seed = 1; seed <= 3; seed++) {
        scene_t sc = base_scene("ref_mix", 25.0f);
        sc.seed = seed;
        sc.music = 1;
        sc.music_to_s = 25.0f;
        sc.speaker_left_only = 1;
        rendered_t r = render_scene(&sc);
        double res[2][2][2];
        for (int build = 0; build < 2; build++) {
            for (int mix = 0; mix < 2; mix++) {
                sim_cfg_t c = base_cfg();
                c.aec4 = build;
                c.aec_ref_channels = 1;
                c.ref_mix = mix ? REF_MIX_LEFT : REF_MIX_DOWNMIX;
                run_t o = run_pipeline(&r, &c);
                erle_stats(&o, 5.0f, 25.0f, &res[build][mix][0], &res[build][mix][1]);
                free_run(&o);
            }
        }
        printf("{\"scenario\":\"ref_mix\",\"seed\":%d,"
               "\"normal_downmix_erle\":%.2f,\"normal_left_erle\":%.2f,"
               "\"aec4_downmix_erle\":%.2f,\"aec4_left_erle\":%.2f,"
               "\"normal_downmix_p10\":%.2f,\"normal_left_p10\":%.2f,"
               "\"aec4_downmix_p10\":%.2f,\"aec4_left_p10\":%.2f}\n",
               seed, res[0][0][0], res[0][1][0], res[1][0][0], res[1][1][0],
               res[0][0][1], res[0][1][1], res[1][0][1], res[1][1][1]);
        free_rendered(&r);
    }
}

/* Talker requests: wake word, then a two part command, three times */
static void add_requests(scene_t *sc, float first_s)
{
    static const int wake_wav[] = { 13, 14, 9 };
    static const int cmd_wav[][2] = { { 11, 17 }, { 12, 15 }, { 16, 18 } };
    sc->n_utt = 0;
    for (int q = 0; q < 3; q++) {
        const float t = first_s + 9.0f * q;
        sc->utt[sc->n_utt++] = (utt_t){ t, wake_wav[q], 1 };
        sc->utt[sc->n_utt++] = (utt_t){ t + 1.8f, cmd_wav[q][0], 0 };
        sc->utt[sc->n_utt++] = (utt_t){ t + 3.0f, cmd_wav[q][1], 0 };
    }
}

/* ch1 output SNR of the wake words (after the first, once steered) and of the commands */
static void request_snr(const rendered_t *r, const run_pair_t *p, double *wake, double *cmd)
{
    double w = 0.0;
    double c = 0.0;
    int nw = 0;
    int nc = 0;
    for (int u = 0; u < 9; u++) {
        const double s = snr_db(&p->a.ch1, &p->b.ch1, r->utt_start[u], r->utt_start[u] + r->utt_len[u]);
        if (u % 3 == 0) {
            if (u > 0) {
                w += s;
                nw++;
            }
        } else {
            c += s;
            nc++;
        }
    }
    *wake = w / nw;
    *cmd = c / nc;
}

/* 3: mic mode: a talker with a TV and noise, PAIR_EW (default) vs BEAM steered by wake words */
static void scenario_mic_mode(void)
{
    static const float talker_az[] = { 60.0f, 150.0f, 300.0f };
    static const float tv_az[] = { 220.0f, 330.0f, 120.0f };
    for (int k = 0; k < 3; k++) {
        scene_t sc = base_scene("mic_mode", 32.0f);
        sc.seed = 10 + k;
        sc.talker_az = talker_az[k];
        sc.tv = 1;
        sc.tv_az = tv_az[k];
        sc.tv_db = -54.0f;
        add_requests(&sc, 4.0f);
        rendered_t r = render_scene(&sc);
        for (int build = 0; build < 2; build++) {
            double wk[3];
            double cm[3];
            static const mic_mode_t modes[] = { MIC_MODE_PAIR_EW, MIC_MODE_AUTO_PAIR, MIC_MODE_BEAM };
            for (int m = 0; m < 3; m++) {
                sim_cfg_t c = base_cfg();
                c.aec4 = build;
                c.aec_ref_channels = 1;
                c.mode = modes[m];
                run_pair_t p = run_pair(&r, &c);
                request_snr(&r, &p, &wk[m], &cm[m]);
                free_pair(&p);
            }
            printf("{\"scenario\":\"mic_mode\",\"build\":\"%s\",\"talker_az\":%.0f,\"tv_az\":%.0f,"
                   "\"wake_snr_pair_ew\":%.2f,\"wake_snr_auto_pair\":%.2f,\"wake_snr_beam\":%.2f,"
                   "\"cmd_snr_pair_ew\":%.2f,\"cmd_snr_auto_pair\":%.2f,\"cmd_snr_beam\":%.2f}\n",
                   build ? "aec4" : "normal", sc.talker_az, sc.tv_az,
                   wk[0], wk[1], wk[2], cm[0], cm[1], cm[2]);
        }
        free_rendered(&r);
    }
}

/* 4: aec4, a wake word while music plays: DOA before the AEC (gated) vs after it */
static void scenario_doa_playback(void)
{
    static const float az[] = { 30.0f, 120.0f, 200.0f, 300.0f };
    static const float music_db[] = { -36.0f, -30.0f };
    for (int lv = 0; lv < 2; lv++) {
        for (int k = 0; k < 4; k++) {
            scene_t sc = base_scene("doa_playback", 32.0f);
            sc.seed = 20 + k + 4 * lv;
            sc.talker_az = az[k];
            sc.music = 1;
            sc.music_to_s = 32.0f;
            sc.music_db = music_db[lv];
            add_requests(&sc, 5.0f);
            rendered_t r = render_scene(&sc);
            double res[2][5];
            for (int post = 0; post < 2; post++) {
                sim_cfg_t c = base_cfg();
                c.aec4 = 1;
                c.mode = MIC_MODE_BEAM;
                c.doa_post_aec = post;
                run_pair_t p = run_pair(&r, &c);
                const run_t o = p.a;
                /* DOA results during the wake words, and where the beam pointed after each */
                int updates = 0;
                int located = 0;
                double err = 0.0;
                for (int q = 0; q < 3; q++) {
                    const size_t a = r.utt_start[3 * q] / FRAME;
                    const size_t b = (r.utt_start[3 * q] + r.utt_len[3 * q]) / FRAME;
                    for (size_t f = a; f < b; f++) {
                        updates += o.doa[f].updated;
                    }
                    const size_t after = (r.utt_start[3 * q + 1]) / FRAME;
                    const unsigned e = ang_err(o.beam[after], (unsigned)sc.talker_az);
                    err += e / 3.0;
                    located += e <= 20;
                }
                double wk;
                double cm;
                request_snr(&r, &p, &wk, &cm);
                res[post][0] = updates / 3.0;
                res[post][1] = located;
                res[post][2] = err;
                res[post][3] = cm;
                res[post][4] = wk;
                free_pair(&p);
            }
            printf("{\"scenario\":\"doa_playback\",\"music_db\":%.0f,\"talker_az\":%.0f,"
                   "\"pre_doa_updates_per_wake\":%.1f,\"post_doa_updates_per_wake\":%.1f,"
                   "\"pre_beam_on_talker\":%.0f,\"post_beam_on_talker\":%.0f,"
                   "\"pre_beam_err_deg\":%.1f,\"post_beam_err_deg\":%.1f,"
                   "\"pre_cmd_snr\":%.2f,\"post_cmd_snr\":%.2f,\"pre_wake_snr\":%.2f,\"post_wake_snr\":%.2f}\n",
                   sc.music_db, sc.talker_az, res[0][0], res[1][0], res[0][1], res[1][1],
                   res[0][2], res[1][2], res[0][3], res[1][3], res[0][4], res[1][4]);
            free_rendered(&r);
        }
    }
}

/* 4b: aec4 post-AEC DOA must not point at the speaker with music alone */
static void scenario_doa_music_only(void)
{
    for (int lv = 0; lv < 2; lv++) {
        scene_t sc = base_scene("doa_music_only", 30.0f);
        sc.seed = 40 + lv;
        sc.music = 1;
        sc.music_to_s = 30.0f;
        sc.music_db = lv ? -24.0f : -30.0f;
        rendered_t r = render_scene(&sc);
        sim_cfg_t c = base_cfg();
        c.aec4 = 1;
        c.mode = MIC_MODE_BEAM;
        c.policy = STEER_POLICY_FOLLOW_SPEECH;
        c.doa_post_aec = 1;
        run_t o = run_pipeline(&r, &c);
        int updates = 0;
        int steers = 0;
        for (size_t f = 1; f < o.frames; f++) {
            updates += o.doa[f].updated;
            steers += o.beam[f] != o.beam[f - 1];
        }
        printf("{\"scenario\":\"doa_music_only\",\"music_db\":%.0f,\"doa_updates\":%d,\"frames\":%zu,"
               "\"beam_steers\":%d}\n", sc.music_db, updates, o.frames, steers);
        free_run(&o);
        free_rendered(&r);
    }
}

/* 5: IC after a re-steer: keep the learned filter vs clear it */
static void scenario_ic_reset(void)
{
    static const float talker_az[] = { 120.0f, 210.0f, 300.0f };
    static const float fan_az[] = { 300.0f, 30.0f, 120.0f };
    for (int k = 0; k < 3; k++) {
        scene_t sc = base_scene("ic_reset", 32.0f);
        sc.seed = 50 + k;
        sc.talker_az = talker_az[k];
        sc.fan_az = fan_az[k];
        sc.fan_db = -58.0f;
        sc.tv = 1;
        sc.tv_az = fmodf(fan_az[k] + 90.0f, 360.0f);
        sc.tv_db = -60.0f;
        add_requests(&sc, 8.0f);
        rendered_t r = render_scene(&sc);
        static const mic_mode_t modes[] = { MIC_MODE_BEAM, MIC_MODE_AUTO_PAIR };
        for (int m = 0; m < 2; m++) {
            for (int build = 0; build < 2; build++) {
                double cmd[2];
                double first_cmd[2];
                double wake[2];
                for (int reset = 0; reset < 2; reset++) {
                    sim_cfg_t c = base_cfg();
                    c.aec4 = build;
                    c.aec_ref_channels = 1;
                    c.mode = modes[m];
                    c.ic_steer_reset = reset;
                    run_pair_t p = run_pair(&r, &c);
                    request_snr(&r, &p, &wake[reset], &cmd[reset]);
                    /* the command right after the first steer */
                    first_cmd[reset] = 0.5 * (snr_db(&p.a.ch1, &p.b.ch1, r.utt_start[1], r.utt_start[1] + r.utt_len[1]) +
                                              snr_db(&p.a.ch1, &p.b.ch1, r.utt_start[2], r.utt_start[2] + r.utt_len[2]));
                    free_pair(&p);
                }
                printf("{\"scenario\":\"ic_reset\",\"mode\":\"%s\",\"build\":\"%s\",\"talker_az\":%.0f,"
                       "\"first_cmd_snr_keep\":%.2f,\"first_cmd_snr_reset\":%.2f,"
                       "\"cmd_snr_keep\":%.2f,\"cmd_snr_reset\":%.2f,"
                       "\"wake_snr_keep\":%.2f,\"wake_snr_reset\":%.2f}\n",
                       m ? "auto_pair" : "beam", build ? "aec4" : "normal", sc.talker_az,
                       first_cmd[0], first_cmd[1], cmd[0], cmd[1], wake[0], wake[1]);
            }
        }
        free_rendered(&r);
    }
}

/* 7: a wake word re-steers the beam while music plays: echo cancellation in both builds */
static void scenario_steer_playback(void)
{
    for (int seed = 1; seed <= 3; seed++) {
        scene_t sc = base_scene("steer_playback", 32.0f);
        sc.seed = 70 + seed;
        sc.talker_az = 90.0f + 60.0f * seed;
        sc.music = 1;
        sc.music_to_s = 32.0f;
        sc.music_db = -34.0f;
        add_requests(&sc, 6.0f);
        rendered_t r = render_scene(&sc);
        sim_cfg_t cn = base_cfg();
        cn.aec_ref_channels = 1;
        cn.mode = MIC_MODE_BEAM;
        cn.policy = STEER_POLICY_WAKE_WORD;
        sim_cfg_t c4 = cn;
        c4.aec4 = 1;
        c4.doa_post_aec = 1;
        /* the normal build cannot locate a wake word during playback: give it the post-AEC steering of aec4 */
        run_pair_t p4 = run_pair(&r, &c4);
        run_pair_t pn = run_pair(&r, &cn);
        const run_t o4 = p4.a;
        const run_t on = pn.a;
        double cmd_n;
        double cmd_4;
        double wk_n;
        double wk_4;
        request_snr(&r, &pn, &wk_n, &cmd_n);
        request_snr(&r, &p4, &wk_4, &cmd_4);
        /* echo on ch1 in the 1.5 s after each wake report, relative to before */
        double dip_n = 0.0;
        double dip_4 = 0.0;
        for (size_t f = 0; f < on.frames; f++) {
            if (r.wake_frame[f]) {
                dip_n += erle_db(&on, f, f + 100) / 3.0;
                dip_4 += erle_db(&o4, f, f + 100) / 3.0;
            }
        }
        printf("{\"scenario\":\"steer_playback\",\"seed\":%d,\"talker_az\":%.0f,"
               "\"erle_after_wake_normal\":%.2f,\"erle_after_wake_aec4\":%.2f,"
               "\"cmd_snr_normal\":%.2f,\"cmd_snr_aec4\":%.2f}\n",
               seed, sc.talker_az, dip_n, dip_4, cmd_n, cmd_4);
        free_pair(&pn);
        free_pair(&p4);
        free_rendered(&r);
    }
}

/* 8: wake word channel tap, NS vs IC vs AEC only, in steady noise (proxy only: no wake word model) */
static void scenario_tap(void)
{
    static const float noise_db[] = { -68.0f, -58.0f };
    for (int k = 0; k < 2; k++) {
        scene_t sc = base_scene("tap", 32.0f);
        sc.seed = 80 + k;
        sc.noise_db = noise_db[k];
        sc.fan_db = noise_db[k] + 4.0f;
        sc.fan_az = 250.0f;
        add_requests(&sc, 4.0f);
        rendered_t r = render_scene(&sc);
        double wk[3];
        double cm[3];
        for (int t = 0; t < 3; t++) {
            sim_cfg_t c = base_cfg();
            c.aec4 = 1;
            c.mode = MIC_MODE_BEAM;
            c.ch1_tap = t;
            run_pair_t p = run_pair(&r, &c);
            request_snr(&r, &p, &wk[t], &cm[t]);
            free_pair(&p);
        }
        printf("{\"scenario\":\"tap\",\"noise_db\":%.0f,\"wake_snr_ns\":%.2f,\"wake_snr_ic\":%.2f,\"wake_snr_aec\":%.2f}\n",
               sc.noise_db, wk[0], wk[1], wk[2]);
        free_rendered(&r);
    }
}

/* 9: what the IC does: noise reduction (idle) and talker SDR, per tap, pair vs beam */
static void scenario_ic_check(void)
{
    scene_t sc = base_scene("ic_check", 32.0f);
    sc.seed = 90;
    sc.noise_db = -62.0f;
    sc.fan_db = -56.0f;
    sc.fan_az = 250.0f;
    add_requests(&sc, 4.0f);
    rendered_t r = render_scene(&sc);
    /* raw North mic: the input SNR */
    {
        asig_t inv = sig_alloc(r.n);
        for (size_t i = 0; i < r.n; i++) {
            inv.x[i] = r.mic[0].x[i] - 2.0f * r.talker[0].x[i];
        }
        printf("{\"scenario\":\"ic_check\",\"what\":\"raw_north\",\"wake_snr\":%.2f}\n",
               snr_db(&r.mic[0], &inv, r.utt_start[3], r.utt_start[3] + r.utt_len[3]));
        sig_free(&inv);
    }
    static const mic_mode_t modes[] = { MIC_MODE_PAIR_EW, MIC_MODE_BEAM };
    for (int m = 0; m < 2; m++) {
        for (int hold = 0; hold < 2; hold++) {
            for (int t = 0; t < 3; t++) {
                sim_cfg_t c = base_cfg();
                c.aec_ref_channels = 1;
                c.mode = modes[m];
                c.ic_fast_hold = hold;
                c.ch1_tap = t;
                run_pair_t p = run_pair(&r, &c);
                const run_t o = p.a;
                double wk;
                double cm;
                request_snr(&r, &p, &wk, &cm);
                /* idle noise level, 2 s before the second request, and talker level */
                const size_t a = r.utt_start[3] - 3 * SIM_FS;
                const double idle = level_db(&o.ch1, a, a + 2 * SIM_FS);
                const double talk = level_db(&o.ch1, r.utt_start[3], r.utt_start[3] + r.utt_len[3]);
                printf("{\"scenario\":\"ic_check\",\"mode\":\"%s\",\"fast_hold\":%d,\"tap\":\"%s\","
                       "\"wake_snr\":%.2f,\"cmd_snr\":%.2f,\"idle_db\":%.2f,\"talk_db\":%.2f}\n",
                       m ? "beam" : "pair_ew", hold, t == 0 ? "ns" : (t == 1 ? "ic" : "aec"),
                       wk, cm, idle, talk);
                free_pair(&p);
            }
        }
    }
    free_rendered(&r);
}

/* debug: noise only, level of the AEC tap vs the raw East mic, HPF on/off */
static void scenario_level_check(void)
{
    scene_t sc = base_scene("level_check", 6.0f);
    sc.seed = 91;
    sc.noise_db = -55.0f;
    rendered_t r = render_scene(&sc);
    printf("{\"what\":\"raw_east\",\"db\":%.2f}\n", level_db(&r.mic[2], SIM_FS, r.n));
    for (int h = 0; h < 2; h++) {
        sim_cfg_t c = base_cfg();
        c.aec_ref_channels = 1;
        c.ch1_tap = 2;
        c.hpf = h;
        c.mic_gain_db = 0;
        run_t o = run_pipeline(&r, &c);
        printf("{\"what\":\"aec_tap\",\"hpf\":%d,\"db\":%.2f}\n", h, level_db(&o.ch1, SIM_FS, r.n));
        free_run(&o);
    }
    free_rendered(&r);
}

/* Quick checks for pytest: the mono reference and the echo gate (~30 s) */
static void scenario_smoke(void)
{
    scene_t sc = base_scene("smoke", 15.0f);
    sc.music = 1;
    sc.music_to_s = 15.0f;
    rendered_t r = render_scene(&sc);
    sim_cfg_t c22 = base_cfg();
    sim_cfg_t c21 = base_cfg();
    c21.aec_ref_channels = 1;
    run_t o22 = run_pipeline(&r, &c22);
    run_t o21 = run_pipeline(&r, &c21);
    double m22, p22, m21, p21;
    erle_stats(&o22, 5.0f, 15.0f, &m22, &p22);
    erle_stats(&o21, 5.0f, 15.0f, &m21, &p21);
    sim_cfg_t c4 = base_cfg();
    c4.aec4 = 1;
    c4.mode = MIC_MODE_BEAM;
    c4.policy = STEER_POLICY_FOLLOW_SPEECH;
    c4.doa_post_aec = 1;
    run_t o4 = run_pipeline(&r, &c4);
    int steers = 0;
    for (size_t f = 1; f < o4.frames; f++) {
        steers += o4.beam[f] != o4.beam[f - 1];
    }
    printf("{\"scenario\":\"smoke\",\"erle_2x2\":%.2f,\"erle_2x1\":%.2f,\"music_only_beam_steers\":%d}\n",
           m22, m21, steers);
    free_run(&o22);
    free_run(&o21);
    free_run(&o4);
    free_rendered(&r);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <speech dir> <scenario|all> [csv dir]\n", argv[0]);
        return 2;
    }
    speech_dir = argv[1];
    csv_dir = argc > 3 ? argv[3] : NULL;
    const char *s = argv[2];
    const int all = !strcmp(s, "all");
    if (all || !strcmp(s, "aec_ref")) scenario_aec_ref();
    if (all || !strcmp(s, "aec_ref_doubletalk")) scenario_aec_ref_doubletalk();
    if (all || !strcmp(s, "ref_mix")) scenario_ref_mix();
    if (all || !strcmp(s, "mic_mode")) scenario_mic_mode();
    if (all || !strcmp(s, "doa_playback")) scenario_doa_playback();
    if (all || !strcmp(s, "doa_music_only")) scenario_doa_music_only();
    if (all || !strcmp(s, "ic_reset")) scenario_ic_reset();
    if (all || !strcmp(s, "steer_playback")) scenario_steer_playback();
    if (all || !strcmp(s, "tap")) scenario_tap();
    if (all || !strcmp(s, "ic_check")) scenario_ic_check();
    if (!strcmp(s, "level_check")) scenario_level_check();
    if (!strcmp(s, "smoke")) scenario_smoke();
    return 0;
}
