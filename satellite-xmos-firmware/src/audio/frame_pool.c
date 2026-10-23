#include <stdint.h>
#include <xcore/assert.h>

#include "audio/frame_pool.h"

void frame_pool_init(frame_pool_t *pool, size_t frame_size, size_t frame_count)
{
    const size_t stride = (frame_size + 7) & ~(size_t)7;
    uint8_t *storage = pvPortMalloc(stride * frame_count);
    xassert(storage != NULL);

    pool->free_frames = xQueueCreate(frame_count, sizeof(void *));
    xassert(pool->free_frames != NULL);

    for (size_t i = 0; i < frame_count; i++) {
        void *frame = &storage[i * stride];
        (void) xQueueSend(pool->free_frames, &frame, 0);
    }
}

void *frame_pool_get(frame_pool_t *pool)
{
    void *frame = NULL;
    (void) xQueueReceive(pool->free_frames, &frame, portMAX_DELAY);
    return frame;
}

void frame_pool_put(frame_pool_t *pool, void *frame)
{
    BaseType_t sent = xQueueSend(pool->free_frames, &frame, 0);
    xassert(sent == pdTRUE);
}
