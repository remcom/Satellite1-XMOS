#include <math.h>
#include <string.h>

#include "audio/beamformer.h"
#include "audio/mic_frontend.h"

#if BEAM_MIC_COUNT != MIC_FRONTEND_MIC_COUNT
#error The beamformer expects MIC_FRONTEND_MIC_COUNT mics
#endif

#define BEAM_PI         3.14159265358979f
#define BEAM_Q30        1073741824.0f
#define BEAM_HISTORY    (BEAM_TAPS - 1)

static const float mic_azimuth_deg[BEAM_MIC_COUNT] = {
    [MIC_IDX_NORTH] = 0.0f,
    [MIC_IDX_SOUTH] = 180.0f,
    [MIC_IDX_EAST]  = 90.0f,
    [MIC_IDX_WEST]  = 270.0f,
};

static int32_t saturate_s32(int64_t v)
{
    if (v > INT32_MAX) {
        return INT32_MAX;
    }
    if (v < -INT32_MAX) {
        return -INT32_MAX;
    }
    return (int32_t)v;
}

/* Blackman-windowed sinc, centred on t = 0 and BEAM_TAPS samples wide. */
static float windowed_sinc(float t)
{
    const float half = 0.5f * (float)BEAM_TAPS;
    if (fabsf(t) >= half) {
        return 0.0f;
    }
    const float x = BEAM_PI * t;
    const float sinc = (fabsf(t) < 1e-6f) ? 1.0f : sinf(x) / x;
    const float w = 0.42f + 0.5f * cosf(BEAM_PI * t / half) + 0.08f * cosf(2.0f * BEAM_PI * t / half);
    return sinc * w;
}

static void compute_weights(beam_weights_t *w, unsigned azimuth_deg)
{
    for (unsigned m = 0; m < BEAM_MIC_COUNT; m++) {
        const float rel = (mic_azimuth_deg[m] - (float)azimuth_deg) * (BEAM_PI / 180.0f);
        /* A mic facing the source hears it early by this much, so it is delayed more */
        const float lead = BEAM_RING_RADIUS_M * cosf(rel) / BEAM_SPEED_OF_SOUND * (float)BEAM_SAMPLE_RATE;
        const float delay = BEAM_DELAY_SAMPLES + lead;

        float h[BEAM_TAPS];
        float sum = 0.0f;
        for (unsigned n = 0; n < BEAM_TAPS; n++) {
            h[n] = windowed_sinc((float)n - delay);
            sum += h[n];
        }
        /* Unity gain at DC, so the delayed mics match exactly at low frequencies */
        for (unsigned n = 0; n < BEAM_TAPS; n++) {
            w->delay_q30[m][BEAM_TAPS - 1 - n] = (int32_t)lrintf(h[n] / sum * BEAM_Q30);
        }
        w->block_q30[m] = (int32_t)lrintf(0.5f * cosf(rel) * BEAM_Q30);
    }
}

void beamformer_init(beamformer_t *bf, unsigned azimuth_deg)
{
    memset(bf, 0, sizeof(*bf));
    beamformer_point(bf, azimuth_deg);
}

void beamformer_point(beamformer_t *bf, unsigned azimuth_deg)
{
    bf->azimuth_deg = (uint16_t)(azimuth_deg % 360);
    compute_weights(&bf->weights, bf->azimuth_deg);
    bf->fade = 0;
}

void beamformer_steer(beamformer_t *bf, unsigned azimuth_deg)
{
    /* A second steer within one frame fades from the beams last heard */
    if (!bf->fade) {
        bf->prev_weights = bf->weights;
    }
    bf->azimuth_deg = (uint16_t)(azimuth_deg % 360);
    compute_weights(&bf->weights, bf->azimuth_deg);
    bf->fade = 1;
}

/* One output sample of both beams. x[m] points at the oldest of the BEAM_TAPS inputs. */
static void beam_sample(const beam_weights_t *w, const int32_t *const x[BEAM_MIC_COUNT],
                        int64_t *target, int64_t *block)
{
    int64_t sum = 0;
    int64_t diff = 0;
    for (unsigned m = 0; m < BEAM_MIC_COUNT; m++) {
        const int32_t *xm = x[m];
        const int32_t *h = w->delay_q30[m];
        int64_t acc = 0;
        for (unsigned k = 0; k < BEAM_TAPS; k++) {
            acc += (int64_t)xm[k] * h[k];
        }
        /*
         * The taps add up to at most 2.2 in absolute value, so neither acc nor
         * the blocking sum below (weights up to 1.42 in total) can overflow.
         */
        const int64_t aligned = acc >> 30;
        sum += aligned;
        diff += aligned * w->block_q30[m];
    }
    *target = sum >> 2;
    *block = diff >> 30;
}

void beamformer_process(beamformer_t *bf,
                        const int32_t *raw,
                        int32_t *out,
                        size_t frame_count)
{
    /* The previous tail followed by the start of this frame, for the first outputs */
    int32_t head[BEAM_MIC_COUNT][2 * BEAM_HISTORY];
    const size_t head_new = (frame_count < BEAM_HISTORY) ? frame_count : BEAM_HISTORY;
    for (unsigned m = 0; m < BEAM_MIC_COUNT; m++) {
        memcpy(head[m], bf->tail[m], sizeof(bf->tail[m]));
        memcpy(&head[m][BEAM_HISTORY], &raw[m * frame_count], head_new * sizeof(int32_t));
    }

    int32_t *target = &out[0];
    int32_t *block = &out[frame_count];
    for (size_t i = 0; i < frame_count; i++) {
        const int32_t *x[BEAM_MIC_COUNT];
        for (unsigned m = 0; m < BEAM_MIC_COUNT; m++) {
            x[m] = (i < BEAM_HISTORY) ? &head[m][i] : &raw[m * frame_count + i - BEAM_HISTORY];
        }

        int64_t t;
        int64_t b;
        beam_sample(&bf->weights, x, &t, &b);
        target[i] = saturate_s32(t);
        block[i] = saturate_s32(b);
        if (bf->fade) {
            /* Linear crossfade from the previous beams, fully new at the last sample */
            beam_sample(&bf->prev_weights, x, &t, &b);
            const int64_t t_prev = saturate_s32(t);
            const int64_t b_prev = saturate_s32(b);
            const int64_t g_q30 = ((int64_t)(i + 1) << 30) / (int64_t)frame_count;
            target[i] = (int32_t)(t_prev + (((target[i] - t_prev) * g_q30) >> 30));
            block[i] = (int32_t)(b_prev + (((block[i] - b_prev) * g_q30) >> 30));
        }
    }
    bf->fade = 0;
    beamformer_feed(bf, raw, frame_count);
}

void beamformer_feed(beamformer_t *bf,
                     const int32_t *raw,
                     size_t frame_count)
{
    for (unsigned m = 0; m < BEAM_MIC_COUNT; m++) {
        const int32_t *frame = &raw[m * frame_count];
        if (frame_count >= BEAM_HISTORY) {
            memcpy(bf->tail[m], &frame[frame_count - BEAM_HISTORY], sizeof(bf->tail[m]));
        } else {
            const size_t keep = BEAM_HISTORY - frame_count;
            memmove(bf->tail[m], &bf->tail[m][frame_count], keep * sizeof(int32_t));
            memcpy(&bf->tail[m][keep], frame, frame_count * sizeof(int32_t));
        }
    }
}
