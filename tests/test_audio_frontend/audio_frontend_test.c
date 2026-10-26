/*
 * Host-side tests for the mic front end and DOA estimator.
 * Built and run by test_audio_frontend.py.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/audio_ctrl.h"
#include "audio/beamformer.h"
#include "audio/doa.h"
#include "audio/mic_frontend.h"
#include "audio/vnr_hold.h"

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

/* A single tone at tone_hz, for measurements per frequency. */
static double tone_hz;
static double tone(double t)
{
    return sin(2.0 * PI * tone_hz * t);
}

/*
 * Fills one frame for a plane wave of signal() from azimuth_deg (0 = North,
 * 90 = East) hitting mics on a ring of the given radius.
 */
static void make_wave(int32_t *mics, unsigned frame_index, double azimuth_deg,
                      double radius, double amplitude, double noise,
                      double (*signal)(double))
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
            double v = amplitude * signal(t - arrival);
            v += noise * (rng_uniform() - 0.5);
            mics[m * DOA_FRAME_SIZE + i] = (int32_t)(v * 2147483647.0);
        }
    }
}

static void make_frame(int32_t *mics, unsigned frame_index, double azimuth_deg,
                       double radius, double amplitude, double noise)
{
    make_wave(mics, frame_index, azimuth_deg, radius, amplitude, noise, source);
}

static double angle_error(double a, double b)
{
    double d = fmod(fabs(a - b), 360.0);
    return d > 180.0 ? 360.0 - d : d;
}

static void test_correlate_matches_reference(void)
{
    static doa_state_t st;
    doa_init(&st);
    doa_complex_t cross[DOA_BIN_COUNT];
    for (int i = 0; i < DOA_BIN_COUNT; i++) {
        cross[i].re = (float)(rng_uniform() - 0.5);
        cross[i].im = (float)(rng_uniform() - 0.5);
    }
    float corr[DOA_CORR_LAGS];
    doa_correlate(&st, cross, corr);

    double max_err = 0;
    for (int l = -DOA_CORR_HALF; l <= DOA_CORR_HALF; l++) {
        double ref = 0;
        for (int i = 0; i < DOA_BIN_COUNT; i++) {
            const double ph = 2.0 * PI * (DOA_BIN_LO + i) * l / DOA_IFFT_N;
            ref += cross[i].re * cos(ph) - cross[i].im * sin(ph);
        }
        max_err = fmax(max_err, fabs(corr[DOA_CORR_HALF + l] - ref));
    }
    CHECK(max_err < 1e-4, "correlation differs from the direct sum by %g", max_err);
}

static void test_mean_square(void)
{
    static int32_t x[1500];
    double ref = 0;
    for (int i = 0; i < 1500; i++) {
        /* quiet (~-80 dBFS) samples, where a plain 2^-30 energy sum would lose precision */
        x[i] = (int32_t)((rng_uniform() - 0.5) * 2.0 * 400000.0);
        const double v = x[i] / 2147483648.0;
        ref += v * v;
    }
    ref /= 1500;
    const double ms = audio_mean_square_s32(x, 1500);
    CHECK(fabs(ms - ref) <= 1e-3 * ref, "mean square %g, expected %g", ms, ref);

    for (int i = 0; i < 8; i++) {
        x[i] = INT32_MIN;
    }
    CHECK(fabs(audio_mean_square_s32(x, 8) - 1.0) < 1e-3, "full scale mean square %g",
          audio_mean_square_s32(x, 8));
    memset(x, 0, sizeof(x));
    CHECK(audio_mean_square_s32(x, 1500) == 0.0f, "silence must have zero energy");
}

static void test_block_energies(void)
{
    /* Two 240 sample channels, the second 20 dB louder, as the AEC reference */
    static int32_t x[2][240];
    double e_ref[2] = {0, 0};
    for (int ch = 0; ch < 2; ch++) {
        for (int i = 0; i < 240; i++) {
            x[ch][i] = (int32_t)((rng_uniform() - 0.5) * 2.0 * (ch ? 4000000.0 : 400000.0));
            const double v = x[ch][i] / 2147483648.0;
            e_ref[ch] += v * v;
        }
    }
    float_s32_t max_e;
    const double ms = audio_block_energies_s32(&x[0][0], 2, 240, &max_e);
    const double ms_ref = audio_mean_square_s32(&x[0][0], 480);
    CHECK(fabs(ms - ms_ref) <= 1e-4 * ms_ref, "mean square %g, audio_mean_square_s32 gives %g", ms, ms_ref);
    const double max_d = ldexp((double)max_e.mant, max_e.exp);
    CHECK(fabs(max_d - e_ref[1]) <= 1e-4 * e_ref[1], "max block energy %g, expected %g", max_d, e_ref[1]);

    memset(x, 0, sizeof(x));
    CHECK(audio_block_energies_s32(&x[0][0], 2, 240, &max_e) == 0.0f && max_e.mant == 0,
          "silence must have zero energy");
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
    const double radii[] = { 0.025, 0.0355 };   /* Satellite1: 7.1 cm between opposite mics */
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

static void test_doa_rejects_implausible_delays(void)
{
    /*
     * 10 cm pairs at 45 degrees: each pair's delay (3.3 samples) is within the
     * search range, but together they are longer than a 7.1 cm ring allows.
     */
    const doa_result_t res = run_doa(45, 0.05, 0);
    CHECK(!res.valid, "delays beyond the ring size gave azimuth %u", res.azimuth_deg);
}

static void test_doa_gated_during_playback(void)
{
    const doa_result_t res = run_doa(90, 0.035, 1);
    CHECK(!res.valid, "DOA must not update while the speaker plays");
}

static void test_doa_playback_hangover(void)
{
    static doa_state_t st;
    static int32_t mics[4 * DOA_FRAME_SIZE];
    doa_init(&st);
    unsigned frame = 0;
    for (; frame < 20; frame++) {
        make_frame(mics, frame, 90, 0.035, 0.0, 1e-4);
        doa_process(&st, mics, 0);
    }
    for (; frame < 40; frame++) {
        make_frame(mics, frame, 90, 0.035, 0.05, 1e-4);
        doa_process(&st, mics, 1);
    }
    /* speaker tail: the estimate stays gated for the hangover after playback stops */
    int first_update = -1;
    for (int i = 0; i < DOA_PLAYBACK_HANGOVER_FRAMES + 5; i++, frame++) {
        make_frame(mics, frame, 90, 0.035, 0.05, 1e-4);
        if (doa_process(&st, mics, 0)->updated && first_update < 0) {
            first_update = i;
        }
    }
    CHECK(first_update == DOA_PLAYBACK_HANGOVER_FRAMES - 1,
          "first DOA %d frames after playback, expected %d", first_update,
          DOA_PLAYBACK_HANGOVER_FRAMES - 1);
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

static void test_average_resets_auto_pair(void)
{
    mic_frontend_state_t st;
    mic_frontend_init(&st);
    const doa_result_t none = {0};
    doa_result_t south = { .azimuth_deg = 180, .confidence = 200, .valid = 1, .updated = 1 };

    for (int i = 0; i < MIC_FRONTEND_AUTO_MIN_DWELL_FRAMES; i++) {
        mic_frontend_update(&st, MIC_MODE_AUTO_PAIR, &none);
    }
    for (int i = 0; i < MIC_FRONTEND_AUTO_STABLE_FRAMES; i++) {
        mic_frontend_update(&st, MIC_MODE_AUTO_PAIR, &south);
    }
    CHECK(st.ch0 == MIC_IDX_SOUTH, "setup: expected South pair, got %u", st.ch0);

    mic_frontend_update(&st, MIC_MODE_AVERAGE, &none);
    mic_frontend_update(&st, MIC_MODE_AUTO_PAIR, &none);
    CHECK(st.ch0 == MIC_IDX_EAST && st.ch1 == MIC_IDX_WEST,
          "AUTO_PAIR after AVERAGE starts from %u/%u, expected East/West", st.ch0, st.ch1);
}

typedef struct {
    double reference;   /* the source at the ring centre, delayed like the beams */
    double target;
    double target_error;
    double block;
} beam_energy_t;

static double db(double ratio)
{
    return 10.0 * log10(ratio);
}

/* A plane wave of signal() from source_deg through beams steered at steer_deg. */
static beam_energy_t run_beam_signal(unsigned steer_deg, double source_deg, double (*signal)(double))
{
    static beamformer_t bf;
    static int32_t mics[4 * DOA_FRAME_SIZE];
    static int32_t out[2 * DOA_FRAME_SIZE];
    const double amplitude = 0.1;
    beamformer_init(&bf, steer_deg);
    beam_energy_t e = {0};
    for (unsigned frame = 0; frame < 6; frame++) {
        make_wave(mics, frame, source_deg, BEAM_RING_RADIUS_M, amplitude, 0.0, signal);
        beamformer_process(&bf, mics, out, DOA_FRAME_SIZE);
        if (frame == 0) {
            continue;   /* the filter history starts out silent */
        }
        for (int i = 0; i < DOA_FRAME_SIZE; i++) {
            const double t = ((double)(frame * DOA_FRAME_SIZE + i) - BEAM_DELAY_SAMPLES) / DOA_SAMPLE_RATE;
            const double ref = amplitude * signal(t);
            const double target = out[i] / 2147483647.0;
            const double block = out[DOA_FRAME_SIZE + i] / 2147483647.0;
            e.reference += ref * ref;
            e.target += target * target;
            e.target_error += (target - ref) * (target - ref);
            e.block += block * block;
        }
    }
    return e;
}

static beam_energy_t run_beam(unsigned steer_deg, double source_deg)
{
    return run_beam_signal(steer_deg, source_deg, source);
}

static void test_beam_steered_direction(void)
{
    double worst_error = -INFINITY;
    double worst_block = -INFINITY;
    for (unsigned az = 0; az < 360; az += 15) {
        const beam_energy_t e = run_beam(az, az);
        /* the target beam passes the talker undistorted ... */
        const double error = db(e.target_error / e.reference);
        /* ... and the blocking beam cancels it */
        const double block = db(e.block / e.reference);
        worst_error = fmax(worst_error, error);
        worst_block = fmax(worst_block, block);
        CHECK(error < -40.0, "steered at %u deg: target error %.1f dB", az, error);
        CHECK(block < -40.0, "steered at %u deg: talker leaks into the blocking beam at %.1f dB", az, block);
    }
    printf("beam on talker: worst target error %.1f dB, worst blocking leak %.1f dB\n",
           worst_error, worst_block);
}

static void test_beam_blocks_only_the_talker(void)
{
    for (unsigned az = 0; az < 360; az += 45) {
        const beam_energy_t behind = run_beam(az, az + 180.0);
        const beam_energy_t side = run_beam(az, az + 90.0);
        const beam_energy_t off = run_beam(az, az + MIC_FRONTEND_BEAM_HOLD_DEG);
        /* the noise reference hears everything except the steered direction */
        CHECK(db(behind.block / behind.reference) > -3.0, "steered at %u deg: source behind at %.1f dB",
              az, db(behind.block / behind.reference));
        CHECK(db(side.block / side.reference) > -9.0, "steered at %u deg: source at the side at %.1f dB",
              az, db(side.block / side.reference));
        const double off_rel = db(off.block / behind.block);
        CHECK(off_rel < -22.0, "steered at %u deg: talker %d deg off reaches the blocking beam at %.1f dB",
              az, MIC_FRONTEND_BEAM_HOLD_DEG, off_rel);
        if (az == 0) {
            printf("blocking beam: behind %.1f dB, side %.1f dB, %d deg off %.1f dB below behind\n",
                   db(behind.block / behind.reference), db(side.block / side.reference),
                   MIC_FRONTEND_BEAM_HOLD_DEG, off_rel);
        }
    }
}

/*
 * Blocking beam gain for a source at source_deg with the beams steered at
 * steer_deg, relative to a single mic, for ideally aligned mics on the ring.
 */
static double ideal_block_db(double freq_hz, double steer_deg, double source_deg)
{
    const double mic_deg[4] = { 0, 180, 90, 270 };
    const double k = 2.0 * PI * freq_hz * BEAM_RING_RADIUS_M / SPEED_OF_SOUND;
    double re = 0.0;
    double im = 0.0;
    for (int m = 0; m < 4; m++) {
        const double steer = (mic_deg[m] - steer_deg) * PI / 180.0;
        const double w = 0.5 * cos(steer);
        const double phase = k * (cos((mic_deg[m] - source_deg) * PI / 180.0) - cos(steer));
        re += w * cos(phase);
        im += w * sin(phase);
    }
    return db(re * re + im * im);
}

static void test_beam_block_bands(void)
{
    /* periods that divide the 1200 measured samples */
    const double freqs[] = { 200, 400, 1000, 2000, 3200, 4000 };
    const double offsets[] = { MIC_FRONTEND_BEAM_HOLD_DEG, 90.0, 180.0 };
    const unsigned steers[] = { 0, 30 };
    for (unsigned s = 0; s < sizeof(steers) / sizeof(steers[0]); s++) {
        const unsigned steer = steers[s];
        printf("blocking beam steered at %u deg, dB vs one mic: Hz, %d deg off, side, behind\n",
               steer, MIC_FRONTEND_BEAM_HOLD_DEG);
        for (unsigned f = 0; f < sizeof(freqs) / sizeof(freqs[0]); f++) {
            tone_hz = freqs[f];
            const beam_energy_t on = run_beam_signal(steer, steer, tone);
            const double on_db = db(on.block / on.reference);
            /* the talker cancels in every band ... */
            CHECK(on_db < -40.0, "%u deg, %.0f Hz: talker leaks into the blocking beam at %.1f dB",
                  steer, tone_hz, on_db);

            /* ... and other directions follow the response described in beamformer.h */
            printf("  %4.0f:", tone_hz);
            for (unsigned o = 0; o < sizeof(offsets) / sizeof(offsets[0]); o++) {
                const beam_energy_t e = run_beam_signal(steer, steer + offsets[o], tone);
                const double got = db(e.block / e.reference);
                const double ideal = ideal_block_db(tone_hz, steer, steer + offsets[o]);
                printf(" %6.1f", got);
                /* deep nulls only need to be deep: there the fractional delays set the depth */
                CHECK(fabs(got - ideal) < 1.0 || (got < -20.0 && ideal < -20.0),
                      "%u deg, %.0f Hz: source %.0f deg off at %.1f dB, ideal %.1f dB",
                      steer, tone_hz, offsets[o], got, ideal);
            }
            printf("\n");
        }
    }
}

static void test_beam_crossfade(void)
{
    static beamformer_t steered;
    static beamformer_t old_beam;
    static beamformer_t new_beam;
    static int32_t mics[4 * DOA_FRAME_SIZE];
    static int32_t out[2 * DOA_FRAME_SIZE];
    static int32_t out_old[2 * DOA_FRAME_SIZE];
    static int32_t out_new[2 * DOA_FRAME_SIZE];
    beamformer_init(&steered, 0);
    beamformer_init(&old_beam, 0);
    beamformer_init(&new_beam, 120);

    for (unsigned frame = 0; frame < 4; frame++) {
        make_frame(mics, frame, 60, BEAM_RING_RADIUS_M, 0.1, 1e-3);
        if (frame == 2) {
            beamformer_steer(&steered, 120);
        }
        beamformer_process(&steered, mics, out, DOA_FRAME_SIZE);
        beamformer_process(&old_beam, mics, out_old, DOA_FRAME_SIZE);
        beamformer_process(&new_beam, mics, out_new, DOA_FRAME_SIZE);

        if (frame < 2) {
            CHECK(memcmp(out, out_old, sizeof(out)) == 0, "frame %u changed before the steer", frame);
        } else if (frame == 2) {
            /* fades from the old beams at the start to the new ones at the end */
            for (int c = 0; c < 2; c++) {
                const int32_t *o = &out[c * DOA_FRAME_SIZE];
                const int32_t *a = &out_old[c * DOA_FRAME_SIZE];
                const int32_t *b = &out_new[c * DOA_FRAME_SIZE];
                CHECK(abs(o[0] - a[0]) <= abs(b[0] - a[0]) / 100 + 1, "ch %d does not start from the old beam", c);
                CHECK(o[DOA_FRAME_SIZE - 1] == b[DOA_FRAME_SIZE - 1], "ch %d does not end on the new beam", c);
            }
        } else {
            CHECK(memcmp(out, out_new, sizeof(out)) == 0, "frame %u is not on the new beams", frame);
        }
    }
    CHECK(steered.azimuth_deg == 120, "steering reads %u", steered.azimuth_deg);
}

static void test_beam_short_frames(void)
{
    /* the filter history works for frames shorter than the filter */
    static beamformer_t whole;
    static beamformer_t pieces;
    static int32_t mics[4 * DOA_FRAME_SIZE];
    static int32_t out[2 * DOA_FRAME_SIZE];
    static int32_t piece_in[4 * 7];
    static int32_t piece_out[2 * 7];
    beamformer_init(&whole, 30);
    beamformer_init(&pieces, 30);
    int mismatches = 0;
    for (unsigned frame = 0; frame < 2; frame++) {
        make_frame(mics, frame, 200, BEAM_RING_RADIUS_M, 0.1, 1e-3);
        beamformer_process(&whole, mics, out, DOA_FRAME_SIZE);
        for (int start = 0; start < DOA_FRAME_SIZE; start += 7) {
            const int n = (DOA_FRAME_SIZE - start < 7) ? DOA_FRAME_SIZE - start : 7;
            for (int m = 0; m < 4; m++) {
                memcpy(&piece_in[m * n], &mics[m * DOA_FRAME_SIZE + start], n * sizeof(int32_t));
            }
            beamformer_process(&pieces, piece_in, piece_out, n);
            for (int i = 0; i < n; i++) {
                mismatches += piece_out[i] != out[start + i];
                mismatches += piece_out[n + i] != out[DOA_FRAME_SIZE + start + i];
            }
        }
    }
    CHECK(mismatches == 0, "%d samples differ between 7-sample and whole frames", mismatches);
}

static void feed_beam_doa(mic_frontend_state_t *st, unsigned azimuth_deg, unsigned frames)
{
    const doa_result_t doa = { .azimuth_deg = (uint16_t)azimuth_deg, .confidence = 200, .valid = 1, .updated = 1 };
    for (unsigned i = 0; i < frames; i++) {
        mic_frontend_update(st, MIC_MODE_BEAM, &doa);
    }
}

static void test_beam_steering(void)
{
    mic_frontend_state_t st;
    mic_frontend_init(&st);
    const doa_result_t none = {0};

    CHECK(mic_frontend_get_beam_azimuth(&st, MIC_MODE_PAIR_EW) == MIC_FRONTEND_NO_BEAM,
          "beam azimuth outside beam mode");
    CHECK(mic_frontend_get_beam_azimuth(&st, MIC_MODE_BEAM) == MIC_FRONTEND_BEAM_INITIAL_DEG,
          "initial beam azimuth %u", mic_frontend_get_beam_azimuth(&st, MIC_MODE_BEAM));
    uint8_t m0;
    uint8_t m1;
    mic_frontend_get_masks(&st, MIC_MODE_BEAM, &m0, &m1);
    CHECK(m0 == 0x0F && m1 == 0x0F, "beam masks %x %x", m0, m1);

    /* entering the mode points the beams at the DOA at once */
    feed_beam_doa(&st, 100, 1);
    CHECK(st.beam.azimuth_deg == 100, "entered the beam mode at %u, expected 100", st.beam.azimuth_deg);
    CHECK(!st.beam.fade, "entering the beam mode must not crossfade");

    /* the DOA must stay away for the stable time; results near the beam restart the count */
    feed_beam_doa(&st, 200, MIC_FRONTEND_AUTO_STABLE_FRAMES - 1);
    feed_beam_doa(&st, 100 + MIC_FRONTEND_BEAM_HOLD_DEG, 1);
    feed_beam_doa(&st, 200, MIC_FRONTEND_AUTO_STABLE_FRAMES - 1);
    CHECK(st.beam.azimuth_deg == 100, "steered before the DOA was stable");
    feed_beam_doa(&st, 200, 1);
    CHECK(st.beam.azimuth_deg == 200, "beam at %u, expected 200", st.beam.azimuth_deg);
    CHECK(st.beam.fade, "a steer must crossfade");

    /* no steering within the dwell time after a steer ... */
    feed_beam_doa(&st, 300, MIC_FRONTEND_AUTO_STABLE_FRAMES + 5);
    CHECK(st.beam.azimuth_deg == 200, "steered within the dwell time");
    for (int i = 0; i < MIC_FRONTEND_AUTO_MIN_DWELL_FRAMES; i++) {
        mic_frontend_update(&st, MIC_MODE_BEAM, &none);
    }
    /* ... but a DOA that stayed away meanwhile moves the beam right after it */
    feed_beam_doa(&st, 300, 1);
    CHECK(st.beam.azimuth_deg == 300, "beam at %u, expected 300", st.beam.azimuth_deg);

    /* a talker between two DOA outcomes gets the mean, across 0 degrees too */
    for (int i = 0; i < MIC_FRONTEND_AUTO_MIN_DWELL_FRAMES; i++) {
        mic_frontend_update(&st, MIC_MODE_BEAM, &none);
    }
    for (int i = 0; i < MIC_FRONTEND_AUTO_STABLE_FRAMES; i++) {
        feed_beam_doa(&st, (i & 1) ? 350 : 10, 1);
    }
    CHECK(st.beam.azimuth_deg == 0, "beam at %u, expected 0", st.beam.azimuth_deg);

    /* two talkers taking turns: their mean points at neither, so the beam stays */
    for (int i = 0; i < MIC_FRONTEND_AUTO_MIN_DWELL_FRAMES; i++) {
        mic_frontend_update(&st, MIC_MODE_BEAM, &none);
    }
    for (int i = 0; i < 3 * MIC_FRONTEND_AUTO_STABLE_FRAMES; i++) {
        feed_beam_doa(&st, (i & 1) ? 260 : 100, 1);
    }
    CHECK(st.beam.azimuth_deg == 0, "two talkers moved the beam to %u", st.beam.azimuth_deg);
    /* ... until one of them keeps talking */
    feed_beam_doa(&st, 100, MIC_FRONTEND_AUTO_STABLE_FRAMES);
    CHECK(st.beam.azimuth_deg == 100, "beam at %u, expected 100", st.beam.azimuth_deg);

    /* DOA results within the hold angle never move it */
    for (int i = 0; i < MIC_FRONTEND_AUTO_MIN_DWELL_FRAMES; i++) {
        mic_frontend_update(&st, MIC_MODE_BEAM, &none);
    }
    feed_beam_doa(&st, 100 - MIC_FRONTEND_BEAM_HOLD_DEG, 3 * MIC_FRONTEND_AUTO_STABLE_FRAMES);
    CHECK(st.beam.azimuth_deg == 100, "moved within the hold angle to %u", st.beam.azimuth_deg);

    /* other modes drop a half counted move; coming back without a DOA keeps the steering */
    feed_beam_doa(&st, 180, MIC_FRONTEND_AUTO_STABLE_FRAMES - 1);
    mic_frontend_update(&st, MIC_MODE_PAIR_EW, &none);
    mic_frontend_update(&st, MIC_MODE_BEAM, &none);
    feed_beam_doa(&st, 180, 1);
    CHECK(st.beam.azimuth_deg == 100, "a mode change kept the count, beam at %u", st.beam.azimuth_deg);

    /* coming back with a DOA points at it, also right after an AUTO_PAIR switch */
    mic_frontend_update(&st, MIC_MODE_PAIR_EW, &none);
    st.frames_since_switch = 0;
    feed_beam_doa(&st, 250, 1);
    CHECK(st.beam.azimuth_deg == 250, "re-entered at %u, expected 250", st.beam.azimuth_deg);
    feed_beam_doa(&st, 40, MIC_FRONTEND_AUTO_STABLE_FRAMES);
    CHECK(st.beam.azimuth_deg == 40, "the dwell of the earlier switch held the beam at %u", st.beam.azimuth_deg);
}

static void test_beam_mode_toggle(void)
{
    /* leaving and re-entering the beam mode gives the beams of a beamformer that ran throughout */
    static mic_frontend_state_t st;
    static beamformer_t throughout;
    static int32_t mics[4 * DOA_FRAME_SIZE];
    static int32_t out[2 * DOA_FRAME_SIZE];
    static int32_t out_throughout[2 * DOA_FRAME_SIZE];
    const doa_result_t doa = { .azimuth_deg = 30, .confidence = 200, .valid = 1, .updated = 1 };
    const mic_mode_t modes[] = {
        MIC_MODE_PAIR_EW, MIC_MODE_PAIR_EW, MIC_MODE_BEAM, MIC_MODE_BEAM,
        MIC_MODE_PAIR_EW, MIC_MODE_BEAM, MIC_MODE_AVERAGE, MIC_MODE_BEAM,
    };
    mic_frontend_init(&st);
    beamformer_init(&throughout, 30);
    for (unsigned frame = 0; frame < sizeof(modes) / sizeof(modes[0]); frame++) {
        make_frame(mics, frame, 30, BEAM_RING_RADIUS_M, 0.1, 1e-3);
        mic_frontend_update(&st, modes[frame], &doa);
        mic_frontend_process(&st, modes[frame], mics, out, DOA_FRAME_SIZE);
        beamformer_process(&throughout, mics, out_throughout, DOA_FRAME_SIZE);
        if (modes[frame] == MIC_MODE_BEAM) {
            CHECK(memcmp(out, out_throughout, sizeof(out)) == 0,
                  "frame %u differs from a beamformer that ran throughout", frame);
        }
    }
}

static void idle_beam(mic_frontend_state_t *st, unsigned frames)
{
    const doa_result_t none = {0};
    for (unsigned i = 0; i < frames; i++) {
        mic_frontend_update(st, MIC_MODE_BEAM, &none);
    }
}

static void test_wake_word_steering(void)
{
    mic_frontend_state_t st;
    mic_frontend_init(&st);
    feed_beam_doa(&st, 0, 1);
    idle_beam(&st, MIC_FRONTEND_AUTO_MIN_DWELL_FRAMES);

    /* a wake word from a new direction steers at once, without the stable time or the dwell */
    feed_beam_doa(&st, 120, 10);
    CHECK(st.beam.azimuth_deg == 0, "speech alone steered too early");
    CHECK(mic_frontend_wake_word(&st, MIC_MODE_BEAM), "the wake word was not located");
    CHECK(st.beam.azimuth_deg == 120, "wake word steered to %u, expected 120", st.beam.azimuth_deg);
    CHECK(st.beam.fade, "a wake word steer must crossfade");
    CHECK(st.wake_azimuth_deg == 120, "wake direction %u", st.wake_azimuth_deg);

    /* ... also right after an earlier steer, and to the mean across 0 degrees */
    idle_beam(&st, MIC_FRONTEND_WAKE_WINDOW_FRAMES);
    for (int i = 0; i < 10; i++) {
        feed_beam_doa(&st, (i & 1) ? 355 : 15, 1);
    }
    CHECK(mic_frontend_wake_word(&st, MIC_MODE_BEAM), "the wake word was not located");
    CHECK(st.beam.azimuth_deg == 5, "wake word steered to %u, expected 5", st.beam.azimuth_deg);

    /* a wake word within the hold angle leaves the beam alone */
    idle_beam(&st, MIC_FRONTEND_WAKE_WINDOW_FRAMES);
    feed_beam_doa(&st, 5 + MIC_FRONTEND_BEAM_HOLD_DEG, 10);
    st.beam.fade = 0;
    CHECK(mic_frontend_wake_word(&st, MIC_MODE_BEAM), "the wake word was not located");
    CHECK(st.beam.azimuth_deg == 5 && !st.beam.fade, "a small correction moved the beam to %u",
          st.beam.azimuth_deg);

    /* too few located frames, or too old ones, give no direction */
    idle_beam(&st, MIC_FRONTEND_WAKE_WINDOW_FRAMES);
    feed_beam_doa(&st, 200, MIC_FRONTEND_WAKE_MIN_RESULTS - 1);
    CHECK(!mic_frontend_wake_word(&st, MIC_MODE_BEAM), "located a wake word from too few results");
    feed_beam_doa(&st, 200, 1);
    CHECK(mic_frontend_wake_word(&st, MIC_MODE_BEAM) && st.beam.azimuth_deg == 200,
          "beam at %u, expected 200", st.beam.azimuth_deg);
    idle_beam(&st, MIC_FRONTEND_WAKE_WINDOW_FRAMES);
    CHECK(!mic_frontend_wake_word(&st, MIC_MODE_BEAM), "used DOA results older than the window");

    /* results spread all around (reverb, several talkers at once) give no direction */
    for (int i = 0; i < 40; i++) {
        feed_beam_doa(&st, (unsigned)(i * 45) % 360, 1);
    }
    CHECK(!mic_frontend_wake_word(&st, MIC_MODE_BEAM) && st.beam.azimuth_deg == 200,
          "scattered results moved the beam to %u", st.beam.azimuth_deg);

    /* the history keeps the most recent results */
    idle_beam(&st, MIC_FRONTEND_WAKE_WINDOW_FRAMES);
    feed_beam_doa(&st, 150, 15);
    feed_beam_doa(&st, 300, MIC_FRONTEND_WAKE_HISTORY);
    CHECK(mic_frontend_wake_word(&st, MIC_MODE_BEAM) && st.beam.azimuth_deg == 300,
          "beam at %u, expected 300", st.beam.azimuth_deg);
}

static void test_wake_word_beside_tv(void)
{
    mic_frontend_state_t st;
    mic_frontend_init(&st);
    feed_beam_doa(&st, 0, 1);

    /* without a TV, the larger group wins */
    idle_beam(&st, MIC_FRONTEND_AUTO_MIN_DWELL_FRAMES);
    feed_beam_doa(&st, 270, 20);
    feed_beam_doa(&st, 60, 35);
    CHECK(mic_frontend_wake_word(&st, MIC_MODE_BEAM) && st.beam.azimuth_deg == 60,
          "beam at %u, expected the wake word at 60", st.beam.azimuth_deg);

    /* a TV that talks all the time loses to a wake word with as many located frames ... */
    mic_frontend_set_steering(&st, STEER_POLICY_WAKE_WORD, 0);
    feed_beam_doa(&st, 200, MIC_FRONTEND_IDLE_DOA_FRAMES);
    feed_beam_doa(&st, 110, MIC_FRONTEND_WAKE_HISTORY / 2);
    feed_beam_doa(&st, 200, MIC_FRONTEND_WAKE_HISTORY / 4);
    CHECK(mic_frontend_wake_word(&st, MIC_MODE_BEAM) && st.beam.azimuth_deg == 110,
          "beam at %u, expected the wake word at 110 beside the TV", st.beam.azimuth_deg);
    /* ... though not to one that hardly produced any */
    feed_beam_doa(&st, 200, MIC_FRONTEND_WAKE_HISTORY - 8);
    feed_beam_doa(&st, 30, 8);
    CHECK(mic_frontend_wake_word(&st, MIC_MODE_BEAM) && st.beam.azimuth_deg == 200,
          "beam at %u, expected the TV to win", st.beam.azimuth_deg);

    /* the talker's own usual place wins over a stray voice with half the frames */
    mic_frontend_init(&st);
    feed_beam_doa(&st, 0, 1);
    feed_beam_doa(&st, 90, MIC_FRONTEND_IDLE_DOA_FRAMES);
    idle_beam(&st, MIC_FRONTEND_WAKE_WINDOW_FRAMES);
    feed_beam_doa(&st, 250, 15);
    feed_beam_doa(&st, 90, 32);
    CHECK(mic_frontend_wake_word(&st, MIC_MODE_BEAM) && st.beam.azimuth_deg == 90,
          "beam at %u, expected the usual talker at 90", st.beam.azimuth_deg);
}

static void test_steer_policies(void)
{
    mic_frontend_state_t st;
    mic_frontend_init(&st);
    feed_beam_doa(&st, 0, 1);
    idle_beam(&st, MIC_FRONTEND_AUTO_MIN_DWELL_FRAMES);

    /* wake word only: a TV talking for minutes does not pull the beam ... */
    mic_frontend_set_steering(&st, STEER_POLICY_WAKE_WORD, 0);
    feed_beam_doa(&st, 200, 20 * MIC_FRONTEND_AUTO_STABLE_FRAMES);
    CHECK(st.beam.azimuth_deg == 0, "the TV pulled the beam to %u", st.beam.azimuth_deg);
    /* ... a wake word does */
    idle_beam(&st, MIC_FRONTEND_WAKE_WINDOW_FRAMES);
    feed_beam_doa(&st, 90, 20);
    CHECK(mic_frontend_wake_word(&st, MIC_MODE_BEAM) && st.beam.azimuth_deg == 90,
          "beam at %u, expected the wake word at 90", st.beam.azimuth_deg);
    /* and re-entering the beam mode returns to the wake word, not to the last DOA */
    mic_frontend_update(&st, MIC_MODE_PAIR_EW, &(doa_result_t){0});
    feed_beam_doa(&st, 200, 1);
    CHECK(st.beam.azimuth_deg == 90, "re-entered at %u, expected the wake word at 90", st.beam.azimuth_deg);

    /* fixed: neither speech nor wake words move it, re-entering keeps it */
    mic_frontend_set_steering(&st, STEER_POLICY_FIXED, 0);
    idle_beam(&st, MIC_FRONTEND_WAKE_WINDOW_FRAMES);
    feed_beam_doa(&st, 300, 3 * MIC_FRONTEND_AUTO_STABLE_FRAMES);
    CHECK(mic_frontend_wake_word(&st, MIC_MODE_BEAM), "the wake word was not located");
    CHECK(st.beam.azimuth_deg == 90, "fixed beam moved to %u", st.beam.azimuth_deg);
    CHECK(st.wake_azimuth_deg == 300, "fixed policy must still record the wake word, %u", st.wake_azimuth_deg);
    mic_frontend_update(&st, MIC_MODE_PAIR_EW, &(doa_result_t){0});
    feed_beam_doa(&st, 300, 1);
    CHECK(st.beam.azimuth_deg == 90, "fixed beam re-entered at %u", st.beam.azimuth_deg);

    /* an unknown policy falls back to following speech */
    mic_frontend_set_steering(&st, STEER_POLICY_COUNT, 0);
    CHECK(st.steer_policy == STEER_POLICY_FOLLOW_SPEECH, "policy %u", st.steer_policy);
}

static void test_conversation_hold(void)
{
    mic_frontend_state_t st;
    mic_frontend_init(&st);
    feed_beam_doa(&st, 0, 1);
    idle_beam(&st, MIC_FRONTEND_AUTO_MIN_DWELL_FRAMES);

    /* following speech, a conversation holds the beam on the talker */
    for (int i = 0; i < 3 * MIC_FRONTEND_AUTO_STABLE_FRAMES; i++) {
        mic_frontend_set_steering(&st, STEER_POLICY_FOLLOW_SPEECH, 1);
        feed_beam_doa(&st, 180, 1);
    }
    CHECK(st.beam.azimuth_deg == 0, "speech re-steered during a conversation to %u", st.beam.azimuth_deg);
    /* ... and the count does not carry over to after it */
    mic_frontend_set_steering(&st, STEER_POLICY_FOLLOW_SPEECH, 0);
    feed_beam_doa(&st, 180, MIC_FRONTEND_AUTO_STABLE_FRAMES - 1);
    CHECK(st.beam.azimuth_deg == 0, "the conversation kept the count");
    feed_beam_doa(&st, 180, 1);
    CHECK(st.beam.azimuth_deg == 180, "beam at %u, expected 180", st.beam.azimuth_deg);

    /* a conversation whose end is never reported lapses */
    idle_beam(&st, MIC_FRONTEND_AUTO_MIN_DWELL_FRAMES);
    for (int i = 0; i < MIC_FRONTEND_CONVERSATION_MAX_FRAMES; i++) {
        mic_frontend_set_steering(&st, STEER_POLICY_FOLLOW_SPEECH, 1);
        idle_beam(&st, 1);
    }
    for (int i = 0; i < MIC_FRONTEND_AUTO_STABLE_FRAMES; i++) {
        mic_frontend_set_steering(&st, STEER_POLICY_FOLLOW_SPEECH, 1);
        feed_beam_doa(&st, 45, 1);
    }
    CHECK(st.beam.azimuth_deg == 45, "an endless conversation held the beam at %u", st.beam.azimuth_deg);
    CHECK(mic_frontend_conversation_lapsed(&st), "the hold did not lapse");

    /* a wake word in that conversation (its end still not reported) restarts the hold */
    mic_frontend_set_steering(&st, STEER_POLICY_FOLLOW_SPEECH, 1);
    (void) mic_frontend_wake_word(&st, MIC_MODE_BEAM);
    CHECK(!mic_frontend_conversation_lapsed(&st) && st.conversation, "a wake word did not restart the hold");
    idle_beam(&st, MIC_FRONTEND_AUTO_MIN_DWELL_FRAMES);
    for (int i = 0; i < 3 * MIC_FRONTEND_AUTO_STABLE_FRAMES; i++) {
        mic_frontend_set_steering(&st, STEER_POLICY_FOLLOW_SPEECH, 1);
        feed_beam_doa(&st, 225, 1);
    }
    CHECK(st.beam.azimuth_deg == 45, "speech re-steered after the second wake word to %u", st.beam.azimuth_deg);

    /* ... but not outside a conversation */
    mic_frontend_set_steering(&st, STEER_POLICY_FOLLOW_SPEECH, 0);
    (void) mic_frontend_wake_word(&st, MIC_MODE_BEAM);
    CHECK(!st.conversation && st.conversation_frames == 0, "a wake word without a conversation held");
}

static void test_wake_word_auto_pair(void)
{
    mic_frontend_state_t st;
    mic_frontend_init(&st);
    const doa_result_t doa = { .azimuth_deg = 170, .confidence = 200, .valid = 1, .updated = 1 };
    mic_frontend_set_steering(&st, STEER_POLICY_WAKE_WORD, 0);
    for (int i = 0; i < 3 * MIC_FRONTEND_AUTO_STABLE_FRAMES; i++) {
        mic_frontend_update(&st, MIC_MODE_AUTO_PAIR, &doa);
    }
    CHECK(st.ch0 == MIC_IDX_EAST, "speech switched the pair under the wake word policy");
    /* the wake word picks the mic facing it, even between two mics */
    const doa_result_t between = { .azimuth_deg = 220, .confidence = 200, .valid = 1, .updated = 1 };
    for (int i = 0; i < 10; i++) {
        mic_frontend_update(&st, MIC_MODE_AUTO_PAIR, &between);
    }
    CHECK(mic_frontend_wake_word(&st, MIC_MODE_AUTO_PAIR), "the wake word was not located");
    CHECK(st.ch0 == MIC_IDX_SOUTH && st.ch1 == MIC_IDX_NORTH, "pair %u/%u, expected S/N", st.ch0, st.ch1);
}

/* Gain of the high-pass at freq_hz in dB, after its transient has settled */
static double hpf_gain_db(double freq_hz)
{
    static int32_t x[2 * DOA_FRAME_SIZE];
    mic_hpf_t hpf;
    mic_hpf_init(&hpf);
    double in = 0;
    double out = 0;
    for (unsigned frame = 0; frame < 60; frame++) {
        for (int i = 0; i < DOA_FRAME_SIZE; i++) {
            const double t = (double)(frame * DOA_FRAME_SIZE + i) / MIC_HPF_SAMPLE_RATE;
            x[i] = (int32_t)(0.5 * sin(2.0 * PI * freq_hz * t) * 2147483647.0);
            x[DOA_FRAME_SIZE + i] = -x[i];
        }
        if (frame >= 40) {
            for (int i = 0; i < DOA_FRAME_SIZE; i++) {
                in += (double)x[i] * x[i];
            }
        }
        mic_hpf_process(&hpf, x, DOA_FRAME_SIZE);
        if (frame >= 40) {
            for (int i = 0; i < DOA_FRAME_SIZE; i++) {
                out += (double)x[i] * x[i];
                CHECK(x[DOA_FRAME_SIZE + i] == -x[i] || abs(x[DOA_FRAME_SIZE + i] + x[i]) <= 1,
                      "channels filtered differently");
            }
        }
    }
    return 10.0 * log10(out / in);
}

static void test_hpf(void)
{
    const double cut = hpf_gain_db(MIC_HPF_CUTOFF_HZ);
    CHECK(fabs(cut + 3.0) < 0.3, "%d Hz: %.2f dB, expected -3 dB", MIC_HPF_CUTOFF_HZ, cut);
    const double low = hpf_gain_db(25);
    CHECK(low < -22.0, "25 Hz: %.2f dB, expected about -24 dB", low);
    const double voice = hpf_gain_db(300);
    CHECK(fabs(voice) < 0.15, "300 Hz: %.2f dB, expected about 0 dB", voice);
    const double high = hpf_gain_db(4000);
    CHECK(fabs(high) < 0.05, "4 kHz: %.2f dB, expected 0 dB", high);
    printf("high-pass: 25 Hz %.1f dB, %d Hz %.1f dB, 300 Hz %.2f dB\n", low, MIC_HPF_CUTOFF_HZ, cut, voice);

    /* DC goes, full scale input saturates instead of wrapping */
    static int32_t x[2 * DOA_FRAME_SIZE];
    mic_hpf_t hpf;
    mic_hpf_init(&hpf);
    for (int frame = 0; frame < 20; frame++) {
        for (int i = 0; i < 2 * DOA_FRAME_SIZE; i++) {
            x[i] = (frame == 0 && i < 8) ? -INT32_MAX : INT32_MAX / 2;
        }
        mic_hpf_process(&hpf, x, DOA_FRAME_SIZE);
    }
    CHECK(abs(x[DOA_FRAME_SIZE - 1]) < 1000, "DC left after 0.3 s: %d", x[DOA_FRAME_SIZE - 1]);
    mic_hpf_reset(&hpf);
    for (int i = 0; i < 2 * DOA_FRAME_SIZE; i++) {
        x[i] = (i & 1) ? INT32_MAX : -INT32_MAX;
    }
    mic_hpf_process(&hpf, x, DOA_FRAME_SIZE);
    int wrapped = 0;
    for (int i = 2; i < DOA_FRAME_SIZE; i++) {
        wrapped += ((i & 1) ? x[i] < 0 : x[i] > 0);
    }
    CHECK(wrapped == 0, "%d samples wrapped around", wrapped);
}

static void test_vnr_hold(void)
{
    /* noise to speech: the fwk_voice EMA needs ~23 frames to reach 0.5, the hold 1..2 */
    vnr_hold_t hold;
    vnr_hold_init(&hold, 0.05f);
    int frames = 0;
    while (vnr_hold_update(&hold, 0.8f) < 0.5f && frames < 100) {
        frames++;
    }
    CHECK(frames + 1 <= 2, "the hold took %d frames to reach 0.5", frames + 1);
    double ema = 0.05;
    int ema_frames = 0;
    while (ema < 0.5 && ema_frames < 100) {
        ema = 0.97 * ema + 0.03 * 0.95;
        ema_frames++;
    }
    CHECK(ema_frames >= 20, "the reference EMA took only %d frames", ema_frames);

    /* falling, it decays like the fwk_voice EMA */
    vnr_hold_init(&hold, 0.9f);
    const float v = vnr_hold_update(&hold, 0.1f);
    CHECK(fabsf(v - (0.97f * 0.9f + 0.03f * 0.1f)) < 1e-6f, "release step gives %f", v);

    /* the leakage above 0.75 waits for the EMA */
    CHECK(vnr_hold_adapt_vnr(0.9f, 0.4f, 0.75f) == 0.75f, "fast estimate leaked before the EMA");
    CHECK(vnr_hold_adapt_vnr(0.9f, 0.8f, 0.75f) == 0.9f, "clamped with the EMA above the threshold");
    CHECK(vnr_hold_adapt_vnr(0.6f, 0.1f, 0.75f) == 0.6f, "clamped below the threshold");
}

static void test_audio_ctrl(void)
{
    audio_ctrl_init(20, MIC_MODE_BEAM);
    CHECK(audio_ctrl_get_ch1_tap() == CH1_TAP_NS, "default tap %u", audio_ctrl_get_ch1_tap());
    CHECK(audio_ctrl_get_ic_fast_hold() == 1 && audio_ctrl_get_hpf() == 1, "fast hold and HPF default on");
    CHECK(audio_ctrl_get_steer_policy() == STEER_POLICY_FOLLOW_SPEECH, "default policy");

    CHECK(audio_ctrl_set_ch1_tap(CH1_TAP_COUNT) != 0, "accepted an unknown tap");
    CHECK(audio_ctrl_set_ch1_tap(CH1_TAP_AEC) == 0, "rejected the AEC tap");
    audio_ctrl_set_ic_fast_hold(0);
    uint32_t w = audio_ctrl_get_t0_word();
    CHECK(AUDIO_CTRL_T0_CH1_TAP(w) == CH1_TAP_AEC && !AUDIO_CTRL_T0_IC_FAST_HOLD(w), "t0 word %x", w);
    audio_ctrl_set_ch1_tap(CH1_TAP_IC);
    audio_ctrl_set_ic_fast_hold(1);
    w = audio_ctrl_get_t0_word();
    CHECK(AUDIO_CTRL_T0_CH1_TAP(w) == CH1_TAP_IC && AUDIO_CTRL_T0_IC_FAST_HOLD(w), "t0 word %x", w);

    CHECK(audio_ctrl_set_steer_policy(STEER_POLICY_COUNT) != 0, "accepted an unknown policy");
    CHECK(audio_ctrl_set_steer_policy(STEER_POLICY_WAKE_WORD) == 0 &&
          audio_ctrl_get_steer_policy() == STEER_POLICY_WAKE_WORD, "policy not set");

    const uint32_t count = audio_ctrl_get_wake_count();
    audio_ctrl_wake_event(1);
    CHECK(audio_ctrl_get_wake_count() == count + 1 && audio_ctrl_get_conversation(), "wake event");
    audio_ctrl_wake_event(0);
    CHECK(audio_ctrl_get_wake_count() == count + 1 && !audio_ctrl_get_conversation(), "end of conversation");

    /* the read reflects the front end's hold, which a new wake word restarts */
    const doa_result_t doa = {0};
    audio_ctrl_wake_event(1);
    CHECK(audio_ctrl_get_conversation_held(), "not held after a wake word");
    audio_ctrl_publish(&doa, 0, 0, MIC_FRONTEND_NO_BEAM, 1);
    CHECK(audio_ctrl_get_conversation() && !audio_ctrl_get_conversation_held(), "held after the hold lapsed");
    audio_ctrl_wake_event(1);
    CHECK(audio_ctrl_get_conversation_held(), "a wake word did not restart the hold");
    audio_ctrl_wake_event(0);
    CHECK(!audio_ctrl_get_conversation_held(), "held after the conversation ended");

    /* taps the build cannot produce are rejected, and an unavailable current tap falls back to NS */
    CHECK(audio_ctrl_set_ch1_tap(CH1_TAP_AEC) == 0, "rejected the AEC tap");
    audio_ctrl_set_ch1_taps(1u << CH1_TAP_IC);
    CHECK(audio_ctrl_get_ch1_tap() == CH1_TAP_NS, "kept an unavailable tap");
    CHECK(audio_ctrl_set_ch1_tap(CH1_TAP_AEC) != 0, "accepted an unavailable tap");
    CHECK(audio_ctrl_set_ch1_tap(CH1_TAP_IC) == 0 && audio_ctrl_set_ch1_tap(CH1_TAP_NS) == 0,
          "rejected an available tap");
    audio_ctrl_set_ch1_taps(0);
    CHECK(audio_ctrl_set_ch1_tap(CH1_TAP_NS) == 0, "rejected the NS tap");
}

static void test_gain(void)
{
    CHECK(mic_gain_db_to_q16(0) == 65536, "0 dB is %d", mic_gain_db_to_q16(0));
    CHECK(abs(mic_gain_db_to_q16(20) - 655360) <= 1, "20 dB is %d", mic_gain_db_to_q16(20));

    int32_t s[4] = { 1000, -1000, INT32_MAX / 4, INT32_MIN / 4 };
    mic_apply_gain_q16(s, 4, mic_gain_db_to_q16(20));
    CHECK(s[0] == 10000 && s[1] == -10000, "x10 gives %d %d", s[0], s[1]);
    /* the VPU saturates symmetrically */
    CHECK(s[2] == INT32_MAX && s[3] == -INT32_MAX, "gain must saturate: %d %d", s[2], s[3]);

    int32_t u[2] = { 12345, -12345 };
    mic_apply_gain_q16(u, 2, mic_gain_db_to_q16(0));
    CHECK(u[0] == 12345 && u[1] == -12345, "0 dB must not change samples: %d %d", u[0], u[1]);

    int32_t w[2] = { 1 << 20, -(1 << 20) };
    const int32_t g30 = mic_gain_db_to_q16(30);
    mic_apply_gain_q16(w, 2, g30);
    const double expect = (double)(1 << 20) * g30 / 65536.0;
    CHECK(fabs(w[0] - expect) <= 16 && fabs(w[1] + expect) <= 16, "30 dB gives %d %d, expected %.0f",
          w[0], w[1], expect);
}

int main(void)
{
    for (int i = 0; i < NUM_TONES; i++) {
        tone_freq[i] = 150.0 + rng_uniform() * 4000.0;
        tone_phase[i] = rng_uniform() * 2.0 * PI;
    }

    test_correlate_matches_reference();
    test_mean_square();
    test_block_energies();
    test_doa_directions();
    test_doa_rejects_implausible_delays();
    test_doa_gated_during_playback();
    test_doa_playback_hangover();
    test_doa_silence_is_invalid();
    test_average_mode();
    test_fixed_pairs();
    test_auto_pair_switching();
    test_average_resets_auto_pair();
    test_beam_steered_direction();
    test_beam_blocks_only_the_talker();
    test_beam_block_bands();
    test_beam_crossfade();
    test_beam_short_frames();
    test_beam_steering();
    test_beam_mode_toggle();
    test_wake_word_steering();
    test_wake_word_beside_tv();
    test_steer_policies();
    test_conversation_hold();
    test_wake_word_auto_pair();
    test_hpf();
    test_vnr_hold();
    test_audio_ctrl();
    test_gain();

    printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
