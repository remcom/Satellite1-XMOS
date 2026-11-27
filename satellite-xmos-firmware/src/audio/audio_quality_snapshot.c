#include "audio/audio_quality.h"

#include <string.h>
#include "rtos_osal.h"
#include "xassert.h"

static rtos_osal_mutex_t snapshot_lock;
/* Only the final output task touches live. Only published is shared. */
static audio_quality_snapshot_t live;
static audio_quality_snapshot_t published;

void audio_quality_init(unsigned output_channels, int reference_available)
{
    memset(&live, 0, sizeof(live));
    live.output_channels = output_channels ? AUDIO_QUALITY_OUTPUT_CHANNELS : 0;
    live.flags = reference_available ? AUDIO_QUALITY_FLAG_REFERENCE : 0;
    published = live;
    const rtos_osal_status_t status = rtos_osal_mutex_create(
        &snapshot_lock, "audio quality", RTOS_OSAL_NOT_RECURSIVE);
    xassert(status == RTOS_OSAL_SUCCESS);
}

void audio_quality_record_output(const int32_t *interleaved, size_t frames)
{
    audio_quality_count_clips(live.clips, interleaved, frames);
}

void audio_quality_publish_frame(const audio_quality_t1_report_t *report)
{
    live.t1 = *report;
    ++live.completed_frames;
    live.flags |= AUDIO_QUALITY_FLAG_READY;
    if (rtos_osal_mutex_get(&snapshot_lock, RTOS_OSAL_NO_WAIT) == RTOS_OSAL_SUCCESS) {
        published = live;
        rtos_osal_mutex_put(&snapshot_lock);
    }
}

void audio_quality_read(uint8_t payload[AUDIO_QUALITY_PAYLOAD_BYTES])
{
    audio_quality_snapshot_t snapshot;
    rtos_osal_mutex_get(&snapshot_lock, RTOS_OSAL_WAIT_FOREVER);
    snapshot = published;
    rtos_osal_mutex_put(&snapshot_lock);
    /* The lock covers only a bounded copy. No math or SPI work under it. */
    audio_quality_encode(payload, &snapshot);
}
