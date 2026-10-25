#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Fixed beams for a generalized sidelobe canceller (GSC) on the Satellite1
 * 4-mic ring.
 *
 * Each mic is delayed by a windowed-sinc fractional delay filter, so a plane
 * wave from the steering azimuth lines up on all four. From the aligned mics:
 *   - target:   their mean (delay-and-sum), passes the steered direction
 *               undistorted
 *   - blocking: sum of cos(mic azimuth - steering) / 2 * aligned mic. The
 *               weights sum to zero, so the steered direction cancels. On the
 *               7.1 cm ring this is a cardioid facing away from the talker.
 *
 * The IC stage uses the blocking beam as its noise reference and so becomes
 * the adaptive half of the GSC.
 *
 * Mic order and azimuth convention follow mic_frontend.h and doa.h: N, S, E, W;
 * 0 degrees points at the North mic, 90 at the East mic.
 */

#define BEAM_MIC_COUNT          4
#define BEAM_TAPS               24
#define BEAM_SAMPLE_RATE        16000
#define BEAM_RING_RADIUS_M      0.0355f /* opposite mics are 7.1 cm apart */
#define BEAM_SPEED_OF_SOUND     343.0f
/* Both beams lag the sound at the ring centre by this many samples (~0.7 ms) */
#define BEAM_DELAY_SAMPLES      ((BEAM_TAPS - 1) * 0.5f)

typedef struct {
    int32_t delay_q30[BEAM_MIC_COUNT][BEAM_TAPS];   /* fractional delay taps, time reversed */
    int32_t block_q30[BEAM_MIC_COUNT];              /* blocking beam weights */
} beam_weights_t;

typedef struct {
    beam_weights_t weights;
    beam_weights_t prev_weights;    /* faded out over the frame after a steer */
    int32_t tail[BEAM_MIC_COUNT][BEAM_TAPS - 1];    /* last input samples, oldest first */
    uint16_t azimuth_deg;
    uint8_t fade;
} beamformer_t;

void beamformer_init(beamformer_t *bf, unsigned azimuth_deg);

/**
 * Steers both beams towards azimuth_deg. The next beamformer_process() call
 * crossfades from the previous beams over its frame.
 */
void beamformer_steer(beamformer_t *bf, unsigned azimuth_deg);

/**
 * \param raw   BEAM_MIC_COUNT channels (N, S, E, W) of frame_count samples
 * \param out   the target beam, then the blocking beam, frame_count samples each
 */
void beamformer_process(beamformer_t *bf,
                        const int32_t *raw,
                        int32_t *out,
                        size_t frame_count);
