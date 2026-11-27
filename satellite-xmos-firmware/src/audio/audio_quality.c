#include "audio/audio_quality.h"

#include <math.h>

static uint32_t saturating_add(uint32_t value, uint32_t increment)
{
    return increment > UINT32_MAX - value ? UINT32_MAX : value + increment;
}

int16_t audio_quality_erle_cdb(float_s32_t input, float_s32_t error)
{
    if (input.mant < 0 || error.mant < 0 || (!input.mant && !error.mant)) {
        return AUDIO_QUALITY_ERLE_UNKNOWN;
    }
    if (!error.mant) return AUDIO_QUALITY_ERLE_LIMIT_CDB;
    if (!input.mant) return -AUDIO_QUALITY_ERLE_LIMIT_CDB;

    /* Work on mantissas and the exponent difference, avoiding overflow or
     * underflow from materializing either energy or their ratio in float. */
    const float cdb = 1000.0f * log10f((float)input.mant / (float)error.mant)
        + 301.029995664f * (float)((int64_t)input.exp - (int64_t)error.exp);
    if (cdb >= AUDIO_QUALITY_ERLE_LIMIT_CDB) return AUDIO_QUALITY_ERLE_LIMIT_CDB;
    if (cdb <= -AUDIO_QUALITY_ERLE_LIMIT_CDB) return -AUDIO_QUALITY_ERLE_LIMIT_CDB;
    return (int16_t)(cdb + (cdb >= 0 ? 0.5f : -0.5f));
}

void audio_quality_count_clips(uint32_t counts[AUDIO_QUALITY_OUTPUT_CHANNELS],
                               const int32_t *interleaved, size_t frames)
{
    uint32_t clips[AUDIO_QUALITY_OUTPUT_CHANNELS] = {0};
    for (size_t i = 0; i < frames; ++i) {
        for (unsigned ch = 0; ch < AUDIO_QUALITY_OUTPUT_CHANNELS; ++ch) {
            const int32_t sample = interleaved[i * AUDIO_QUALITY_OUTPUT_CHANNELS + ch];
            if (sample >= AUDIO_QUALITY_CLIP_THRESHOLD || sample <= -AUDIO_QUALITY_CLIP_THRESHOLD) {
                ++clips[ch];
            }
        }
    }
    for (unsigned ch = 0; ch < AUDIO_QUALITY_OUTPUT_CHANNELS; ++ch) {
        counts[ch] = saturating_add(counts[ch], clips[ch]);
    }
}

void audio_quality_account_reference_drops(audio_quality_ref_drops_t *counts,
                                           int oldest_discarded, int newest_discarded)
{
    counts->oldest = saturating_add(counts->oldest, oldest_discarded != 0);
    counts->newest = saturating_add(counts->newest, newest_discarded != 0);
}

static void put_u16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *p, uint32_t value)
{
    put_u16(p, (uint16_t)value);
    put_u16(p + 2, (uint16_t)(value >> 16));
}

void audio_quality_encode(uint8_t payload[AUDIO_QUALITY_PAYLOAD_BYTES],
                          const audio_quality_snapshot_t *snapshot)
{
    const unsigned channels = snapshot->t1.aec_channels <= AUDIO_QUALITY_MAX_AEC_CHANNELS
        ? snapshot->t1.aec_channels : AUDIO_QUALITY_MAX_AEC_CHANNELS;
    payload[0] = AUDIO_QUALITY_VERSION;
    payload[1] = (uint8_t)channels;
    payload[2] = snapshot->output_channels;
    payload[3] = snapshot->flags;
    put_u32(payload + 4, snapshot->completed_frames);
    put_u32(payload + 8, snapshot->t1.ref_drops.oldest);
    put_u32(payload + 12, snapshot->t1.ref_drops.newest);
    for (unsigned ch = 0; ch < AUDIO_QUALITY_MAX_AEC_CHANNELS; ++ch) {
        const int16_t erle = ch < channels
            ? audio_quality_erle_cdb(snapshot->t1.input_energy[ch], snapshot->t1.error_energy[ch])
            : AUDIO_QUALITY_ERLE_UNKNOWN;
        put_u16(payload + 16 + 2 * ch, (uint16_t)erle);
    }
    for (unsigned ch = 0; ch < AUDIO_QUALITY_OUTPUT_CHANNELS; ++ch) {
        put_u32(payload + 24 + 4 * ch, snapshot->clips[ch]);
    }
}
