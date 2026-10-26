#include "audio/audio_ctrl.h"

/*
 * mic_gain_q16/mic_gain_db and mic_mode are separate words, so a frame may run
 * with the new gain and the old mode (or the old q16 and the new dB readback).
 * That lasts at most one frame and is harmless; no lock is taken on purpose.
 */
static volatile uint32_t mic_gain_db;
static volatile int32_t mic_gain_q16;
static volatile uint32_t mic_mode;

static volatile uint32_t ch1_tap;
static volatile uint32_t ic_fast_hold;
static volatile uint32_t hpf;
static volatile uint32_t steer_policy;
static volatile uint32_t conversation;
/* Incremented after conversation is set by a wake word */
static volatile uint32_t wake_count;

/* azimuth[15:0] | confidence[23:16] | valid[24] */
static volatile uint32_t doa_word;
/* Incremented after doa_word whenever the DOA produced a new result */
static volatile uint32_t doa_update_count;
/* ch0_mask[7:0] | ch1_mask[15:8] | beam_azimuth[31:16] */
static volatile uint32_t frontend_word;

void audio_ctrl_init(unsigned gain_db, mic_mode_t mode)
{
    if (audio_ctrl_set_mic_gain_db(gain_db) != 0) {
        (void) audio_ctrl_set_mic_gain_db(0);
    }
    if (audio_ctrl_set_mic_mode(mode) != 0) {
        (void) audio_ctrl_set_mic_mode(MIC_MODE_PAIR_EW);
    }
    ch1_tap = CH1_TAP_NS;
    ic_fast_hold = 1;
    hpf = 1;
    steer_policy = STEER_POLICY_FOLLOW_SPEECH;
    conversation = 0;
    wake_count = 0;
    doa_word = 0;
    doa_update_count = 0;
    frontend_word = MIC_MASK(MIC_IDX_EAST) | (MIC_MASK(MIC_IDX_WEST) << 8) |
                    ((uint32_t)MIC_FRONTEND_NO_BEAM << 16);
}

int audio_ctrl_set_mic_gain_db(unsigned gain_db)
{
    if (gain_db > AUDIO_CTRL_MIC_GAIN_DB_MAX) {
        return -1;
    }
    mic_gain_q16 = mic_gain_db_to_q16(gain_db);
    mic_gain_db = gain_db;
    return 0;
}

unsigned audio_ctrl_get_mic_gain_db(void)
{
    return mic_gain_db;
}

int32_t audio_ctrl_get_mic_gain_q16(void)
{
    return mic_gain_q16;
}

int audio_ctrl_set_mic_mode(unsigned mode)
{
    if (mode >= MIC_MODE_COUNT) {
        return -1;
    }
    mic_mode = mode;
    return 0;
}

mic_mode_t audio_ctrl_get_mic_mode(void)
{
    return (mic_mode_t) mic_mode;
}

int audio_ctrl_set_ch1_tap(unsigned tap)
{
    if (tap >= CH1_TAP_COUNT) {
        return -1;
    }
    ch1_tap = tap;
    return 0;
}

ch1_tap_t audio_ctrl_get_ch1_tap(void)
{
    return (ch1_tap_t) ch1_tap;
}

void audio_ctrl_set_ic_fast_hold(unsigned enable)
{
    ic_fast_hold = enable ? 1 : 0;
}

unsigned audio_ctrl_get_ic_fast_hold(void)
{
    return ic_fast_hold;
}

uint32_t audio_ctrl_get_t0_word(void)
{
    return (ch1_tap & 0xFF) | (ic_fast_hold << 8);
}

void audio_ctrl_set_hpf(unsigned enable)
{
    hpf = enable ? 1 : 0;
}

unsigned audio_ctrl_get_hpf(void)
{
    return hpf;
}

int audio_ctrl_set_steer_policy(unsigned policy)
{
    if (policy >= STEER_POLICY_COUNT) {
        return -1;
    }
    steer_policy = policy;
    return 0;
}

steer_policy_t audio_ctrl_get_steer_policy(void)
{
    return (steer_policy_t) steer_policy;
}

void audio_ctrl_wake_event(unsigned active)
{
    conversation = active ? 1 : 0;
    if (active) {
        wake_count = wake_count + 1;
    }
}

uint32_t audio_ctrl_get_wake_count(void)
{
    return wake_count;
}

unsigned audio_ctrl_get_conversation(void)
{
    return conversation;
}

void audio_ctrl_publish(const doa_result_t *doa, uint8_t ch0_mask, uint8_t ch1_mask,
                        uint16_t beam_azimuth_deg)
{
    doa_word = (uint32_t)doa->azimuth_deg |
               ((uint32_t)doa->confidence << 16) |
               ((uint32_t)(doa->valid ? 1 : 0) << 24);
    if (doa->updated) {
        doa_update_count = doa_update_count + 1;
    }
    frontend_word = (uint32_t)ch0_mask | ((uint32_t)ch1_mask << 8) |
                    ((uint32_t)beam_azimuth_deg << 16);
}

doa_result_t audio_ctrl_get_doa(void)
{
    const uint32_t w = doa_word;
    doa_result_t doa = {
        .azimuth_deg = (uint16_t)(w & 0xFFFF),
        .confidence = (uint8_t)((w >> 16) & 0xFF),
        .valid = (uint8_t)((w >> 24) & 1),
    };
    return doa;
}

uint32_t audio_ctrl_get_doa_update_count(void)
{
    return doa_update_count;
}

void audio_ctrl_get_mic_masks(uint8_t *ch0_mask, uint8_t *ch1_mask)
{
    const uint32_t w = frontend_word;
    *ch0_mask = (uint8_t)(w & 0xFF);
    *ch1_mask = (uint8_t)((w >> 8) & 0xFF);
}

uint16_t audio_ctrl_get_beam_azimuth(void)
{
    return (uint16_t)(frontend_word >> 16);
}
