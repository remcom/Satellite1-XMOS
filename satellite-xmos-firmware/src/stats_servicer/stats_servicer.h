#pragma once

#include "servicer.h"

#define STATS_SERVICER_RESID              (250)
#define NUM_RESOURCES_STATS_SERVICER      (  1)

/**
 * @brief Stats servicer task.
 *
 * Reports audio stage processing times, stack and heap use of both tiles
 * (audio/stage_stats.h). Runs on tile 0, which receives the tile 1 numbers
 * with the audio frames.
 *
 * \param args   Pointer to the servicer_register_ctx_t of this servicer
 */
void stats_servicer(void *args);

/**
 * @brief Stats servicer initialisation function.
 * \param servicer      Pointer to the Servicer's state data structure
 */
void stats_servicer_init(servicer_t *servicer);
