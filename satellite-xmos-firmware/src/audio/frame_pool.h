#pragma once

#include <stddef.h>

#include "FreeRTOS.h"
#include "queue.h"

/*
 * Fixed set of equally sized audio frames, allocated once at start-up.
 * Pipelines take a frame per audio block and return it when done instead of
 * allocating and freeing heap memory for every block.
 */
typedef struct {
    QueueHandle_t free_frames;
} frame_pool_t;

/** Allocates frame_count frames of frame_size bytes (8-byte aligned). */
void frame_pool_init(frame_pool_t *pool, size_t frame_size, size_t frame_count);

/** Takes a frame, blocking until one is returned if all are in use. */
void *frame_pool_get(frame_pool_t *pool);

void frame_pool_put(frame_pool_t *pool, void *frame);
