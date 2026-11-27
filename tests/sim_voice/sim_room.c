#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim_room.h"

#define PI 3.14159265358979f
#define SPEED_OF_SOUND 343.0f
#define RING_RADIUS 0.0355f
#define SINC_HALF 16
#define MAX_ORDER 14

/* Mic offsets from the ring centre: N = +y, E = +x */
static const float mic_off[SIM_MICS][3] = {
    { 0.0f,  RING_RADIUS, 0.0f },   /* N */
    { 0.0f, -RING_RADIUS, 0.0f },   /* S */
    { RING_RADIUS, 0.0f, 0.0f },    /* E */
    { -RING_RADIUS, 0.0f, 0.0f },   /* W */
};

asig_t sig_alloc(size_t n)
{
    asig_t s = { calloc(n, sizeof(float)), n };
    if (!s.x) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    return s;
}

void sig_free(asig_t *s)
{
    free(s->x);
    s->x = NULL;
    s->n = 0;
}

void sig_add(asig_t *dst, const asig_t *src, size_t offset, float gain)
{
    for (size_t i = 0; i < src->n && offset + i < dst->n; i++) {
        dst->x[offset + i] += gain * src->x[i];
    }
}

float sig_rms(const asig_t *s, size_t start, size_t len)
{
    double e = 0.0;
    size_t cnt = 0;
    for (size_t i = start; i < start + len && i < s->n; i++, cnt++) {
        e += (double)s->x[i] * s->x[i];
    }
    return cnt ? (float)sqrt(e / (double)cnt) : 0.0f;
}

void sig_scale_to_rms(asig_t *s, float rms)
{
    const float cur = sig_rms(s, 0, s->n);
    if (cur <= 0.0f) {
        return;
    }
    for (size_t i = 0; i < s->n; i++) {
        s->x[i] *= rms / cur;
    }
}

asig_t sig_load_wav(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        exit(1);
    }
    unsigned char hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
        fprintf(stderr, "%s: not a wav\n", path);
        exit(1);
    }
    asig_t s = { NULL, 0 };
    for (;;) {
        unsigned char ch[8];
        if (fread(ch, 1, 8, f) != 8) {
            break;
        }
        const uint32_t len = ch[4] | (ch[5] << 8) | (ch[6] << 16) | ((uint32_t)ch[7] << 24);
        if (!memcmp(ch, "data", 4)) {
            s = sig_alloc(len / 2);
            for (size_t i = 0; i < s.n; i++) {
                unsigned char b[2];
                if (fread(b, 1, 2, f) != 2) {
                    break;
                }
                s.x[i] = (float)(int16_t)(b[0] | (b[1] << 8)) / 32768.0f;
            }
            break;
        }
        fseek(f, len + (len & 1), SEEK_CUR);
    }
    fclose(f);
    if (!s.x) {
        fprintf(stderr, "%s: no data\n", path);
        exit(1);
    }
    return s;
}

float room_rt60(const room_t *r)
{
    const float v = r->room[0] * r->room[1] * r->room[2];
    const float a = 2.0f * (r->room[0] * r->room[1] + r->room[0] * r->room[2] + r->room[1] * r->room[2]);
    return 0.161f * v / (a * (1.0f - r->beta * r->beta));
}

void room_source_pos(float azimuth_deg, float distance, float height, float pos[3])
{
    const float a = azimuth_deg * PI / 180.0f;
    pos[0] = distance * sinf(a);
    pos[1] = distance * cosf(a);
    pos[2] = height;
}

static void add_fractional_impulse(float *rir, float delay, float amp)
{
    const int d0 = (int)floorf(delay);
    for (int k = -SINC_HALF + 1; k <= SINC_HALF; k++) {
        const int n = d0 + k;
        if (n < 0 || n >= SIM_RIR_LEN) {
            continue;
        }
        const float t = (float)n - delay;
        const float x = PI * t;
        const float sinc = fabsf(t) < 1e-6f ? 1.0f : sinf(x) / x;
        const float w = 0.5f + 0.5f * cosf(PI * t / (float)SINC_HALF);
        rir[n] += amp * sinc * w;
    }
}

void room_rir(const room_t *r, const float src_rel[3], unsigned mic, float *rir)
{
    memset(rir, 0, SIM_RIR_LEN * sizeof(float));
    float src[3];
    float rcv[3];
    for (int d = 0; d < 3; d++) {
        src[d] = r->device[d] + src_rel[d];
        rcv[d] = r->device[d] + mic_off[mic][d];
        if (src[d] < 0.05f || src[d] > r->room[d] - 0.05f) {
            fprintf(stderr, "source outside the room (axis %d: %.2f m)\n", d, src[d]);
            exit(1);
        }
    }
    const float max_dist = (float)(SIM_RIR_LEN - SINC_HALF) / SIM_FS * SPEED_OF_SOUND;
    for (int nx = -MAX_ORDER; nx <= MAX_ORDER; nx++) {
        for (int ny = -MAX_ORDER; ny <= MAX_ORDER; ny++) {
            for (int nz = -MAX_ORDER; nz <= MAX_ORDER; nz++) {
                const int n[3] = { nx, ny, nz };
                for (int p = 0; p < 8; p++) {
                    float img[3];
                    int refl = 0;
                    for (int d = 0; d < 3; d++) {
                        const int q = (p >> d) & 1;
                        /* image coordinate: 2 n L +- src */
                        img[d] = 2.0f * (float)n[d] * r->room[d] + (q ? -src[d] : src[d]);
                        refl += abs(2 * n[d] - q);
                    }
                    const float dx = img[0] - rcv[0];
                    const float dy = img[1] - rcv[1];
                    const float dz = img[2] - rcv[2];
                    const float dist = sqrtf(dx * dx + dy * dy + dz * dz);
                    if (dist > max_dist) {
                        continue;
                    }
                    const float amp = powf(r->beta, (float)refl) / (4.0f * PI * fmaxf(dist, 0.01f));
                    add_fractional_impulse(rir, dist / SPEED_OF_SOUND * SIM_FS, amp);
                }
            }
        }
    }
    /*
     * Same-sign reflections pile up at DC, which real (frequency dependent)
     * walls do not do: high-pass the response like Allen & Berkley (1979).
     * Second order Butterworth at 60 Hz.
     */
    const float k = tanf(PI * 60.0f / SIM_FS);
    const float q = 0.70710678f;
    const float norm = 1.0f / (1.0f + k / q + k * k);
    const float b0 = norm, b1 = -2.0f * norm, b2 = norm;
    const float a1 = 2.0f * (k * k - 1.0f) * norm, a2 = (1.0f - k / q + k * k) * norm;
    float x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    for (int n = 0; n < SIM_RIR_LEN; n++) {
        const float x0 = rir[n];
        const float y0 = b0 * x0 + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x0; y2 = y1; y1 = y0;
        rir[n] = y0;
    }
}

void room_render(const room_t *r, const float src_rel[3], const asig_t *src, asig_t out[SIM_MICS])
{
    float *rir = malloc(SIM_RIR_LEN * sizeof(float));
    for (unsigned m = 0; m < SIM_MICS; m++) {
        room_rir(r, src_rel, m, rir);
        /* only the non-zero span of the RIR */
        int first = 0;
        while (first < SIM_RIR_LEN && rir[first] == 0.0f) {
            first++;
        }
        for (size_t i = 0; i < out[m].n; i++) {
            float acc = 0.0f;
            const int kmax = (i < SIM_RIR_LEN) ? (int)i : SIM_RIR_LEN - 1;
            for (int k = first; k <= kmax; k++) {
                const size_t j = i - (size_t)k;
                if (j < src->n) {
                    acc += rir[k] * src->x[j];
                }
            }
            out[m].x[i] += acc;
        }
    }
    free(rir);
}

float sim_rand(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return (float)(x >> 8) / 8388608.0f - 1.0f;
}

void music_synth(asig_t *left, asig_t *right, float section_s, float width, uint32_t seed)
{
    uint32_t rs = seed;
    const size_t n = left->n;
    const size_t section = (size_t)(section_s * SIM_FS);
    float pan[3] = { 0.5f, 0.5f, 0.5f };
    float phase[3][6] = { { 0 } };
    float f0[3] = { 220.0f, 330.0f, 110.0f };
    float env[3] = { 0 };
    size_t note_left[3] = { 0, 0, 0 };
    float perc_env = 0.0f;
    float lp_l = 0.0f;
    float lp_r = 0.0f;
    static const float scale[] = { 1.0f, 1.122f, 1.26f, 1.335f, 1.498f, 1.682f, 1.888f, 2.0f };

    for (size_t i = 0; i < n; i++) {
        if (i % section == 0) {
            for (int v = 0; v < 3; v++) {
                pan[v] = 0.5f + 0.5f * width * sim_rand(&rs);
            }
        }
        float l = 0.0f;
        float r = 0.0f;
        for (int v = 0; v < 3; v++) {
            if (note_left[v] == 0) {
                const float base = (v == 2) ? 82.4f : (v == 1 ? 261.6f : 196.0f);
                f0[v] = base * scale[(unsigned)((sim_rand(&rs) + 1.0f) * 3.99f)];
                note_left[v] = (size_t)((0.2f + 0.15f * (sim_rand(&rs) + 1.0f)) * SIM_FS);
                env[v] = 1.0f;
            }
            note_left[v]--;
            env[v] *= 0.99985f;
            float s = 0.0f;
            for (int h = 0; h < 6; h++) {
                phase[v][h] += 2.0f * PI * f0[v] * (float)(h + 1) / SIM_FS;
                if (phase[v][h] > 2.0f * PI) {
                    phase[v][h] -= 2.0f * PI;
                }
                s += sinf(phase[v][h]) / (float)(h + 1);
            }
            s *= env[v];
            const float a = pan[v] * PI / 2.0f;
            l += cosf(a) * s;
            r += sinf(a) * s;
        }
        if (i % (SIM_FS / 4) == 0) {
            perc_env = 1.0f;
        }
        perc_env *= 0.9992f;
        const float perc = 0.4f * perc_env * sim_rand(&rs);
        /* decorrelated "ambience" */
        lp_l = 0.9f * lp_l + 0.1f * sim_rand(&rs);
        lp_r = 0.9f * lp_r + 0.1f * sim_rand(&rs);
        left->x[i] = l + perc + 0.15f * width * lp_l;
        right->x[i] = r + perc + 0.15f * width * lp_r;
    }
}

void noise_synth(asig_t *s, uint32_t seed)
{
    /* Pink noise (-3 dB/octave), Paul Kellet's filter: room noise without a low frequency bias */
    uint32_t rs = seed;
    float b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
    for (size_t i = 0; i < s->n; i++) {
        const float w = sim_rand(&rs);
        b0 = 0.99886f * b0 + w * 0.0555179f;
        b1 = 0.99332f * b1 + w * 0.0750759f;
        b2 = 0.96900f * b2 + w * 0.1538520f;
        b3 = 0.86650f * b3 + w * 0.3104856f;
        b4 = 0.55000f * b4 + w * 0.5329522f;
        b5 = -0.7616f * b5 - w * 0.0168980f;
        s->x[i] = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362f;
        b6 = w * 0.115926f;
    }
}

void speaker_nonlinearity(asig_t *s, float drive)
{
    if (drive <= 0.0f) {
        return;
    }
    for (size_t i = 0; i < s->n; i++) {
        s->x[i] = tanhf(drive * s->x[i]) / drive;
    }
}
