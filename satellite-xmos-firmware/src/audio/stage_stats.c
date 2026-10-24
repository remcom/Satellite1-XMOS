#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "audio/stage_stats.h"

#define TICKS_PER_US        (XS1_TIMER_HZ / 1000000)
/* Scanning the stack for its watermark is not free, so it is sampled every this many runs */
#define STACK_SAMPLE_PERIOD 64

/*
 * Each value is a single word written by one task. A max update racing with a
 * reset can be lost; that is acceptable for diagnostics, so no lock is taken.
 */
static volatile uint32_t last_ticks[STAGE_STATS_COUNT];
static volatile uint32_t max_ticks[STAGE_STATS_COUNT];
static volatile uint32_t stack_free_words[STAGE_STATS_COUNT];
static uint32_t run_count[STAGE_STATS_COUNT];
static volatile uint32_t t1_heap_free;
static volatile uint32_t t1_heap_min_free;

void stage_stats_init(void)
{
    for (unsigned i = 0; i < STAGE_STATS_COUNT; i++) {
        last_ticks[i] = 0;
        max_ticks[i] = 0;
        stack_free_words[i] = STAGE_STATS_STACK_UNKNOWN;
        run_count[i] = 0;
    }
}

void stage_stats_end(stage_stats_id_t id, uint32_t start)
{
    const uint32_t ticks = get_reference_time() - start;
    last_ticks[id] = ticks;
    if (ticks > max_ticks[id]) {
        max_ticks[id] = ticks;
    }
    if (run_count[id]++ % STACK_SAMPLE_PERIOD == STACK_SAMPLE_PERIOD - 1) {
        stack_free_words[id] = uxTaskGetStackHighWaterMark(NULL);
    }
}

void stage_stats_make_t1_report(stage_stats_t1_report_t *report)
{
    for (unsigned i = 0; i < STAGE_STATS_T1_COUNT; i++) {
        report->last_ticks[i] = last_ticks[i];
        report->max_ticks[i] = max_ticks[i];
        max_ticks[i] = 0;
        report->stack_free_words[i] = stack_free_words[i];
    }
    report->heap_free = xPortGetFreeHeapSize();
    report->heap_min_free = xPortGetMinimumEverFreeHeapSize();
}

void stage_stats_merge_t1_report(const stage_stats_t1_report_t *report)
{
    for (unsigned i = 0; i < STAGE_STATS_T1_COUNT; i++) {
        last_ticks[i] = report->last_ticks[i];
        if (report->max_ticks[i] > max_ticks[i]) {
            max_ticks[i] = report->max_ticks[i];
        }
        stack_free_words[i] = report->stack_free_words[i];
    }
    t1_heap_free = report->heap_free;
    t1_heap_min_free = report->heap_min_free;
}

static uint16_t ticks_to_us(uint32_t ticks)
{
    const uint32_t us = ticks / TICKS_PER_US;
    return (us > UINT16_MAX) ? UINT16_MAX : (uint16_t)us;
}

void stage_stats_read_times_us(uint16_t last_us[STAGE_STATS_COUNT], uint16_t max_us[STAGE_STATS_COUNT])
{
    for (unsigned i = 0; i < STAGE_STATS_COUNT; i++) {
        last_us[i] = ticks_to_us(last_ticks[i]);
        max_us[i] = ticks_to_us(max_ticks[i]);
        max_ticks[i] = 0;
    }
}

void stage_stats_read_stack_free(uint16_t free_words[STAGE_STATS_COUNT])
{
    for (unsigned i = 0; i < STAGE_STATS_COUNT; i++) {
        const uint32_t w = stack_free_words[i];
        free_words[i] = (w > UINT16_MAX) ? UINT16_MAX : (uint16_t)w;
    }
}

void stage_stats_read_heap(uint32_t heap[4])
{
    heap[0] = xPortGetFreeHeapSize();
    heap[1] = xPortGetMinimumEverFreeHeapSize();
    heap[2] = t1_heap_free;
    heap[3] = t1_heap_min_free;
}
