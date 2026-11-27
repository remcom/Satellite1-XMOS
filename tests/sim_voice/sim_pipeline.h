#pragma once

#include <stdint.h>

#include "audio/doa.h"
#include "audio/mic_frontend.h"

#define SIM_FRAME 240

typedef enum {
    REF_MIX_DOWNMIX = 0,    /* (L+R)/2 */
    REF_MIX_LEFT = 1,
    REF_MIX_RIGHT = 2,
} sim_ref_mix_t;

typedef struct {
    int aec4;               /* AEC on the four raw mics, mono reference */
    int aec_ref_channels;   /* 2-mic build: 2 (L, R) or 1 (ref_mix) */
    sim_ref_mix_t ref_mix;  /* mono reference: what it is mixed from */
    int doa_post_aec;       /* aec4: DOA on the echo cancelled mics, echo gate instead of the playback gate */
    int ic_steer_reset;     /* experiment: clear the IC filter when the channels change */
    int ic_fast_hold;
    mic_mode_t mode;
    steer_policy_t policy;
    int ch1_tap;            /* 0 NS, 1 IC, 2 AEC */
    unsigned mic_gain_db;
    int hpf;
} sim_cfg_t;

typedef struct {
    int32_t ch0[SIM_FRAME];     /* AEC+IC+NS (no AGC) */
    int32_t ch1[SIM_FRAME];     /* by ch1_tap */
    float aec_in_energy;        /* mean square over the AEC channels */
    float aec_out_energy;
    doa_result_t doa;
    uint16_t beam_azimuth;
    uint8_t ch0_mic;            /* mic feeding ch0 in the pair modes */
    int doa_gated;              /* the DOA treated this frame as echo */
} sim_out_t;

void sim_pipeline_init(const sim_cfg_t *cfg);

/**
 * \param raw        4 mics (N, S, E, W) x SIM_FRAME, Q31
 * \param ref_l      speaker reference, left (16 kHz, Q31)
 * \param ref_r      right
 * \param vnr        voice-to-noise probability of this frame (oracle, 0..1)
 * \param wake       a wake word was reported this frame
 * \param conversation a conversation is active
 */
void sim_pipeline_frame(const int32_t raw[4][SIM_FRAME],
                        const int32_t *ref_l, const int32_t *ref_r,
                        float vnr, int wake, int conversation,
                        sim_out_t *out);
