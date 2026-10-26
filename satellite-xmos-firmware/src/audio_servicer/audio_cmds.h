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
  AUDIO_SERVICER_CMD_CH1_TAP,       // read/write, 1 byte: right output channel, ch1_tap_t (0 = AEC+IC+NS, 1 = AEC+IC, 2 = AEC)
  AUDIO_SERVICER_CMD_IC_FAST_HOLD,  // read/write, 1 byte: 1 = IC stops adapting within ~2 frames of speech onset, 0 = fwk_voice timing
  AUDIO_SERVICER_CMD_HPF,           // read/write, 1 byte: 1 = 100 Hz high-pass before the AEC, 0 = off
  AUDIO_SERVICER_CMD_STEER_POLICY,  // read/write, 1 byte: steer_policy_t (0 = follow speech, 1 = wake word only, 2 = fixed)
  AUDIO_SERVICER_CMD_WAKE_EVENT,    // read/write, 1 byte: write 1 = wake word detected now, 0 = conversation ended; reads the conversation state

  NUM_AUDIO_SERVICER_CMDS
};

// AUDIO_SERVICER_CMD_DOA flags
#define AUDIO_SERVICER_DOA_FLAG_VALID    (1 << 0)
#define AUDIO_SERVICER_DOA_FLAG_UPDATED  (1 << 1)  // a new result since the previous DOA read
