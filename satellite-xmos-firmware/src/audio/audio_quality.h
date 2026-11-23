#pragma once

#include <stddef.h>
#include <stdint.h>
#include "xmath/types.h"

#define AUDIO_QUALITY_VERSION 1
#define AUDIO_QUALITY_PAYLOAD_BYTES 32
#define AUDIO_QUALITY_MAX_AEC_CHANNELS 4
#define AUDIO_QUALITY_OUTPUT_CHANNELS 2
#define AUDIO_QUALITY_ERLE_UNKNOWN INT16_MIN
#define AUDIO_QUALITY_ERLE_LIMIT_CDB 9600
/* ceil(0.99 * 2^31), applied symmetrically without abs(INT32_MIN). */
#define AUDIO_QUALITY_CLIP_THRESHOLD INT32_C(2126008812)
#define AUDIO_QUALITY_FLAG_REFERENCE 1u
#define AUDIO_QUALITY_FLAG_READY 2u

typedef struct {
    uint32_t oldest;
    uint32_t newest;
} audio_quality_ref_drops_t;

/* Carried with the audio frame; never expose native struct layout on SPI. */
typedef struct {
    float_s32_t input_energy[AUDIO_QUALITY_MAX_AEC_CHANNELS];
    float_s32_t error_energy[AUDIO_QUALITY_MAX_AEC_CHANNELS];
    audio_quality_ref_drops_t ref_drops;
    uint32_t aec_channels;
} audio_quality_t1_report_t;

typedef struct {
    audio_quality_t1_report_t t1;
    uint32_t completed_frames;
    uint32_t clips[AUDIO_QUALITY_OUTPUT_CHANNELS];
    uint8_t output_channels;
    uint8_t flags;
} audio_quality_snapshot_t;

int16_t audio_quality_erle_cdb(float_s32_t input, float_s32_t error);
void audio_quality_count_clips(uint32_t counts[AUDIO_QUALITY_OUTPUT_CHANNELS],
                               const int32_t *interleaved, size_t frames);
void audio_quality_account_reference_drops(audio_quality_ref_drops_t *counts,
                                           int oldest_discarded, int newest_discarded);
void audio_quality_encode(uint8_t payload[AUDIO_QUALITY_PAYLOAD_BYTES],
                          const audio_quality_snapshot_t *snapshot);

/* Tile 0: initialize before starting the servicer/pipeline. One output writer,
 * one stats reader. The writer never waits for the reader: it skips publishing
 * while the snapshot is being copied. Counters still accumulate on every frame.
 */
void audio_quality_init(unsigned output_channels, int reference_available);
void audio_quality_record_output(const int32_t *interleaved, size_t frames);
void audio_quality_publish_frame(const audio_quality_t1_report_t *report);
void audio_quality_read(uint8_t payload[AUDIO_QUALITY_PAYLOAD_BYTES]);
