#pragma once

#include <stdint.h>

// LED_RING_SERVICER_RESID commands
enum e_led_ring_servicer_cmds
{
  LED_RING_SERVICER_CMD_WRITE_RAW  = 0,
  LED_RING_SERVICER_CMD_DOA_INDICATOR,  // write, 4 bytes: enable, then one LED's 3 bytes in WRITE_RAW byte order

  NUM_LED_RING_SERVICER_CMDS
};

