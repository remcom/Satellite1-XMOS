#pragma once

#include <stdint.h>
#include <xcore/hwtimer.h>

/*
 * Processing time and stack use of the audio stages, to check the real-time
 * headroom on hardware. Times are in reference clock ticks (100 MHz).
 *
 * Tile 1 reports its stages with every frame it sends to tile 0, where the
 * stats servicer reads all of them.
 */
typedef enum {
    /* tile 1 */
    STAGE_STATS_T1_FRONTEND = 0,    /* mic front end and DOA */
    STAGE_STATS_T1_AEC,             /* AEC and mic gain */
    STAGE_STATS_T1_SPK_SRC,         /* speaker reference, 48 to 16 kHz */
    /* tile 0 */
    STAGE_STATS_T0_IC_VNR,
    STAGE_STATS_T0_NS,
    STAGE_STATS_T0_AGC,
    STAGE_STATS_T0_OUT_SRC,         /* pipeline output, 16 to 48 kHz */

    STAGE_STATS_COUNT
} stage_stats_id_t;

#define STAGE_STATS_T1_COUNT    (STAGE_STATS_T1_SPK_SRC + 1)

/* Stack free while unknown: the stage has not run, or not long enough to be sampled */
#define STAGE_STATS_STACK_UNKNOWN   UINT32_MAX

/* Tile 1 numbers, carried in every frame to tile 0 */
typedef struct {
    uint32_t last_ticks[STAGE_STATS_T1_COUNT];
    uint32_t max_ticks[STAGE_STATS_T1_COUNT];      /* since the previous report */
    uint32_t stack_free_words[STAGE_STATS_T1_COUNT];
    uint32_t heap_free;
    uint32_t heap_min_free;
} stage_stats_t1_report_t;

void stage_stats_init(void);

static inline uint32_t stage_stats_start(void)
{
    return get_reference_time();
}

/** Records one run of a stage that began at start. Call it from the task running the stage. */
void stage_stats_end(stage_stats_id_t id, uint32_t start);

/** Tile 1: fills a report and restarts the max times. */
void stage_stats_make_t1_report(stage_stats_t1_report_t *report);

/** Tile 0: merges a report from tile 1. */
void stage_stats_merge_t1_report(const stage_stats_t1_report_t *report);

/**
 * Tile 0, for the stats servicer: per stage the last time and the max time
 * since the previous call, in microseconds (saturating). Restarts the max times.
 */
void stage_stats_read_times_us(uint16_t last_us[STAGE_STATS_COUNT], uint16_t max_us[STAGE_STATS_COUNT]);

/** Tile 0: lowest free stack seen per stage, in words, 0xFFFF while unknown. */
void stage_stats_read_stack_free(uint16_t free_words[STAGE_STATS_COUNT]);

/** Tile 0: current and lowest free heap in bytes of tile 0, then tile 1. */
void stage_stats_read_heap(uint32_t heap[4]);
