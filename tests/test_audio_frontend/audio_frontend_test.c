/*
 * Host-side tests for the mic front end and DOA estimator.
 * Built and run by test_audio_frontend.py.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/doa.h"
#include "audio/mic_frontend.h"

#define PI 3.14159265358979
#define SPEED_OF_SOUND 343.0
#define NUM_TONES 160

static int failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        failures++; \
        printf("FAIL %s:%d: ", __FILE__, __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while (0)

static double tone_freq[NUM_TONES];
static double tone_phase[NUM_TONES];

/* Deterministic pseudo random numbers, so results do not depend on the libc. */
static unsigned rng_state = 12345;
static double rng_uniform(void)
{
    rng_state = rng_state * 1103515245u + 12345u;
    return (double)((rng_state >> 8) & 0xFFFFFF) / (double)0x1000000;
}

/* Broadband test signal that can be delayed by any fraction of a sample. */
static double source(double t)
{
    double v = 0.0;
    for (int i = 0; i < NUM_TONES; i++) {
        v += sin(2.0 * PI * tone_freq[i] * t + tone_phase[i]);
    }
    return v / sqrt((double)NUM_TONES);
}

/*
 * Fills one frame for a plane wave from azimuth_deg (0 = North, 90 = East)
 * hitting mics on a ring of the given radius.
 */
static void make_frame(int32_t *mics, unsigned frame_index, double azimuth_deg,
                       double radius, double amplitude, double noise)
{
    /* mic positions (x = East, y = North), order N, S, E, W */
    const double pos[4][2] = { {0, radius}, {0, -radius}, {radius, 0}, {-radius, 0} };
    const double az = azimuth_deg * PI / 180.0;
    const double ux = sin(az);
    const double uy = cos(az);

    for (int m = 0; m < 4; m++) {
        /* mics closer to the source hear it earlier */
        const double arrival = -(pos[m][0] * ux + pos[m][1] * uy) / SPEED_OF_SOUND;
        for (int i = 0; i < DOA_FRAME_SIZE; i++) {
            const double t = (double)(frame_index * DOA_FRAME_SIZE + i) / DOA_SAMPLE_RATE;
            double v = amplitude * source(t - arrival);
            v += noise * (rng_uniform() - 0.5);
            mics[m * DOA_FRAME_SIZE + i] = (int32_t)(v * 2147483647.0);
        }
    }
}

static double angle_error(double a, double b)
{
    double d = fmod(fabs(a - b), 360.0);
    return d > 180.0 ? 360.0 - d : d;
}

static void test_fft_matches_dft(void)
{
    static doa_state_t st;
    doa_init(&st);
    doa_complex_t x[64];
    doa_complex_t ref[64];
    for (int i = 0; i < 64; i++) {
        x[i].re = (float)(rng_uniform() - 0.5);
        x[i].im = (float)(rng_uniform() - 0.5);
    }
    for (int k = 0; k < 64; k++) {
        double re = 0;
        double im = 0;
        for (int n = 0; n < 64; n++) {
            const double ph = -2.0 * PI * k * n / 64.0;
            re += x[n].re * cos(ph) - x[n].im * sin(ph);
            im += x[n].re * sin(ph) + x[n].im * cos(ph);
        }
        ref[k].re = (float)re;
        ref[k].im = (float)im;
    }
    doa_fft(x, 64, st.twiddle, 0);
    double max_err = 0;
    for (int k = 0; k < 64; k++) {
        max_err = fmax(max_err, fabs(x[k].re - ref[k].re));
        max_err = fmax(max_err, fabs(x[k].im - ref[k].im));
    }
    CHECK(max_err < 1e-4, "FFT differs from DFT by %g", max_err);
}

/* Runs a quiet lead-in (to settle the noise floor) and then speech from one direction. */
static doa_result_t run_doa(double azimuth_deg, double radius, int playback)
{
    static doa_state_t st;
    static int32_t mics[4 * DOA_FRAME_SIZE];
    doa_init(&st);
    doa_result_t last = {0};
    unsigned frame = 0;
    for (; frame < 20; frame++) {
        make_frame(mics, frame, azimuth_deg, radius, 0.0, 1e-4);
        doa_process(&st, mics, playback);
    }
    for (; frame < 40; frame++) {
        make_frame(mics, frame, azimuth_deg, radius, 0.05, 1e-4);
        last = *doa_process(&st, mics, playback);
    }
    return last;
}

static void test_doa_directions(void)
{
    const double radii[] = { 0.025, 0.035, 0.05 };
    for (unsigned r = 0; r < sizeof(radii) / sizeof(radii[0]); r++) {
        double worst = 0;
        for (int az = 0; az < 360; az += 15) {
            const doa_result_t res = run_doa(az, radii[r], 0);
            CHECK(res.valid && res.updated, "no DOA for %d deg, radius %.3f", az, radii[r]);
            const double err = angle_error(res.azimuth_deg, az);
            worst = fmax(worst, err);
            CHECK(err <= 10.0, "azimuth %d deg estimated as %u (radius %.3f, conf %u)",
                  az, res.azimuth_deg, radii[r], res.confidence);
        }
        printf("radius %.3f m: worst azimuth error %.1f deg\n", radii[r], worst);
    }
}

static void test_doa_gated_during_playback(void)
{
    const doa_result_t res = run_doa(90, 0.035, 1);
    CHECK(!res.valid, "DOA must not update while the speaker plays");
}

static void test_doa_silence_is_invalid(void)
{
    static doa_state_t st;
    static int32_t mics[4 * DOA_FRAME_SIZE];
    doa_init(&st);
    doa_result_t res = {0};
    for (unsigned frame = 0; frame < 50; frame++) {
        make_frame(mics, frame, 0, 0.035, 0.0, 1e-4);
        res = *doa_process(&st, mics, 0);
    }
    CHECK(!res.valid, "noise only must not produce a DOA");
}

static void test_average_mode(void)
{
    mic_frontend_state_t st;
    mic_frontend_init(&st);
    int32_t raw[4 * 2] = {
        100, INT32_MAX,     /* N */
        -100, INT32_MIN,    /* S */
        300, INT32_MAX,     /* E */
        -300, INT32_MIN,    /* W */
    };
    int32_t out[2 * 2];
    mic_frontend_process(&st, MIC_MODE_AVERAGE, raw, out, 2);
    CHECK(out[0] == 200 && out[2] == -200, "average of N+E / S+W: %d %d", out[0], out[2]);
    CHECK(out[1] == INT32_MAX - 1 && out[3] == INT32_MIN, "average must not overflow: %d %d",
          out[1], out[3]);

    uint8_t m0;
    uint8_t m1;
    mic_frontend_get_masks(&st, MIC_MODE_AVERAGE, &m0, &m1);
    CHECK(m0 == (MIC_MASK(MIC_IDX_NORTH) | MIC_MASK(MIC_IDX_EAST)), "average ch0 mask %x", m0);
}

static void test_fixed_pairs(void)
{
    mic_frontend_state_t st;
    mic_frontend_init(&st);
    int32_t raw[4] = { 1, 2, 3, 4 };    /* N S E W, one sample each */
    int32_t out[2];
    const doa_result_t none = {0};

    mic_frontend_update(&st, MIC_MODE_PAIR_EW, &none);
    mic_frontend_process(&st, MIC_MODE_PAIR_EW, raw, out, 1);
    CHECK(out[0] == 3 && out[1] == 4, "EW pair gives %d %d", out[0], out[1]);

    mic_frontend_update(&st, MIC_MODE_PAIR_NS, &none);
    mic_frontend_process(&st, MIC_MODE_PAIR_NS, raw, out, 1);
    CHECK(out[0] == 1 && out[1] == 2, "NS pair gives %d %d", out[0], out[1]);
}

static void test_auto_pair_switching(void)
{
    mic_frontend_state_t st;
    mic_frontend_init(&st);
    const doa_result_t none = {0};
    doa_result_t south = { .azimuth_deg = 185, .confidence = 200, .valid = 1, .updated = 1 };
    doa_result_t boundary = { .azimuth_deg = 225, .confidence = 200, .valid = 1, .updated = 1 };

    mic_frontend_update(&st, MIC_MODE_PAIR_EW, &none);

    /* switching needs the dwell time first */
    for (int i = 0; i < MIC_FRONTEND_AUTO_STABLE_FRAMES + 5; i++) {
        mic_frontend_update(&st, MIC_MODE_AUTO_PAIR, &south);
    }
    CHECK(st.ch0 == MIC_IDX_EAST, "switched before the dwell time");

    for (int i = 0; i < MIC_FRONTEND_AUTO_MIN_DWELL_FRAMES; i++) {
        mic_frontend_update(&st, MIC_MODE_AUTO_PAIR, &none);
    }
    /* directions between two mics do not count */
    for (int i = 0; i < 2 * MIC_FRONTEND_AUTO_STABLE_FRAMES; i++) {
        mic_frontend_update(&st, MIC_MODE_AUTO_PAIR, &boundary);
    }
    CHECK(st.ch0 == MIC_IDX_EAST, "switched on a boundary direction");

    for (int i = 0; i < MIC_FRONTEND_AUTO_STABLE_FRAMES; i++) {
        mic_frontend_update(&st, MIC_MODE_AUTO_PAIR, &south);
    }
    CHECK(st.ch0 == MIC_IDX_SOUTH && st.ch1 == MIC_IDX_NORTH,
          "expected South/North pair, got %u/%u", st.ch0, st.ch1);

    /* an immediate change of direction is held off by the dwell time */
    doa_result_t east = { .azimuth_deg = 90, .confidence = 200, .valid = 1, .updated = 1 };
    for (int i = 0; i < MIC_FRONTEND_AUTO_STABLE_FRAMES; i++) {
        mic_frontend_update(&st, MIC_MODE_AUTO_PAIR, &east);
    }
    CHECK(st.ch0 == MIC_IDX_SOUTH, "switched again within the dwell time");
}

static void test_gain(void)
{
    CHECK(mic_gain_db_to_q16(0) == 65536, "0 dB is %d", mic_gain_db_to_q16(0));
    CHECK(abs(mic_gain_db_to_q16(20) - 655360) <= 1, "20 dB is %d", mic_gain_db_to_q16(20));

    int32_t s[4] = { 1000, -1000, INT32_MAX / 4, INT32_MIN / 4 };
    mic_apply_gain_q16(s, 4, mic_gain_db_to_q16(20));
    CHECK(s[0] == 10000 && s[1] == -10000, "x10 gives %d %d", s[0], s[1]);
    CHECK(s[2] == INT32_MAX && s[3] == INT32_MIN, "gain must saturate: %d %d", s[2], s[3]);
}

int main(void)
{
    for (int i = 0; i < NUM_TONES; i++) {
        tone_freq[i] = 150.0 + rng_uniform() * 4000.0;
        tone_phase[i] = rng_uniform() * 2.0 * PI;
    }

    test_fft_matches_dft();
    test_doa_directions();
    test_doa_gated_during_playback();
    test_doa_silence_is_invalid();
    test_average_mode();
    test_fixed_pairs();
    test_auto_pair_switching();
    test_gain();

    printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
