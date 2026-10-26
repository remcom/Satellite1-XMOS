#pragma once

#include "servicer.h"

#define AUDIO_SERVICER_RESID              (230)
#define NUM_RESOURCES_AUDIO_SERVICER      (  1)

/**
 * @brief Audio servicer task.
 *
 * Handles the mic, DOA, output channel and beam steering commands (audio_cmds.h)
 * from the device control interface.
 * Runs on the mic array tile, next to the audio pipeline state it controls.
 *
 * \param args   Pointer to the servicer_register_ctx_t of this servicer
 */
void audio_servicer(void *args);

/**
 * @brief Audio servicer initialisation function.
 * \param servicer      Pointer to the Servicer's state data structure
 */
void audio_servicer_init(servicer_t *servicer);
