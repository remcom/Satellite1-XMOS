#include "audio/audio_ctrl.h"

static volatile uint32_t mic_gain_db;
static volatile int32_t mic_gain_q16;
static volatile uint32_t mic_mode;

/* azimuth[15:0] | confidence[23:16] | valid[24] | updated[25] */
static volatile uint32_t doa_word;
/* ch0_mask[7:0] | ch1_mask[15:8] */
static volatile uint32_t mic_masks_word;

void audio_ctrl_init(unsigned gain_db, mic_mode_t mode)
{
    if (audio_ctrl_set_mic_gain_db(gain_db) != 0) {
        (void) audio_ctrl_set_mic_gain_db(0);
    }
    if (audio_ctrl_set_mic_mode(mode) != 0) {
        (void) audio_ctrl_set_mic_mode(MIC_MODE_PAIR_EW);
    }
    doa_word = 0;
    mic_masks_word = MIC_MASK(MIC_IDX_EAST) | (MIC_MASK(MIC_IDX_WEST) << 8);
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

void audio_ctrl_publish(const doa_result_t *doa, uint8_t ch0_mask, uint8_t ch1_mask)
{
    doa_word = (uint32_t)doa->azimuth_deg |
               ((uint32_t)doa->confidence << 16) |
               ((uint32_t)(doa->valid ? 1 : 0) << 24) |
               ((uint32_t)(doa->updated ? 1 : 0) << 25);
    mic_masks_word = (uint32_t)ch0_mask | ((uint32_t)ch1_mask << 8);
}

doa_result_t audio_ctrl_get_doa(void)
{
    const uint32_t w = doa_word;
    doa_result_t doa = {
        .azimuth_deg = (uint16_t)(w & 0xFFFF),
        .confidence = (uint8_t)((w >> 16) & 0xFF),
        .valid = (uint8_t)((w >> 24) & 1),
        .updated = (uint8_t)((w >> 25) & 1),
    };
    return doa;
}

void audio_ctrl_get_mic_masks(uint8_t *ch0_mask, uint8_t *ch1_mask)
{
    const uint32_t w = mic_masks_word;
    *ch0_mask = (uint8_t)(w & 0xFF);
    *ch1_mask = (uint8_t)((w >> 8) & 0xFF);
}
