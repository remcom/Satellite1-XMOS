#pragma once

#include "audio/stage_stats.h"

// STATS_SERVICER_RESID commands, all read only. Values are little-endian.
enum e_stats_servicer_cmds
{
  STATS_SERVICER_CMD_STAGE_TIMES = 0,   // 2 * STAGE_STATS_COUNT uint16: last us per stage, then max us per stage since the previous read
  STATS_SERVICER_CMD_STACK_FREE,        // STAGE_STATS_COUNT uint16: lowest free stack per stage in words, 0xFFFF unknown
  STATS_SERVICER_CMD_HEAP_FREE,         // 4 uint32: tile 0 free, tile 0 lowest free, tile 1 free, tile 1 lowest free (bytes)

  NUM_STATS_SERVICER_CMDS
};
