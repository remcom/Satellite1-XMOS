#pragma once

#include <stdint.h>

// AUDIO_SERVICER_RESID commands
enum e_audio_servicer_cmds
{
  AUDIO_SERVICER_CMD_MIC_GAIN = 0,  // read/write, 1 byte: mic gain in dB (0..30)
  AUDIO_SERVICER_CMD_MIC_MODE,      // read/write, 1 byte: mic_mode_t
  AUDIO_SERVICER_CMD_DOA,           // read, 4 bytes: azimuth LSB, azimuth MSB, confidence, flags
  AUDIO_SERVICER_CMD_MIC_PAIR,      // read, 2 bytes: channel 0 mic mask, channel 1 mic mask
  AUDIO_SERVICER_CMD_BEAM,          // read, 2 bytes: beam azimuth LSB, MSB (0xFFFF: not in beam mode)

  NUM_AUDIO_SERVICER_CMDS
};

// AUDIO_SERVICER_CMD_DOA flags
#define AUDIO_SERVICER_DOA_FLAG_VALID    (1 << 0)
#define AUDIO_SERVICER_DOA_FLAG_UPDATED  (1 << 1)  // a new result since the previous DOA read
