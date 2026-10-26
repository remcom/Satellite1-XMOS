#pragma once

/*
 * Voice-to-noise estimate for the IC hold decision, smoothed with a fast attack
 * and a slow release.
 *
 * fwk_voice smooths the VNR of the IC input with an EMA (alpha 0.97, ~33
 * frames) and holds the IC once that crosses 0.5. From noise (~0.05) into
 * speech (0.8..0.95) that takes 23..30 frames, 340..450 ms: the IC adapts
 * through the first half of a wake word, and with the talker in its reference
 * (a mic pair, or a mis-steered beam) it learns to cancel the talker. Rising
 * estimates here follow within one or two frames; falling ones decay as
 * slowly as the fwk_voice EMA, so the hold after speech is unchanged.
 *
 * fwk_voice uses the same VNR for the leakage it applies to the IC filter
 * above input_vnr_threshold_high (0.75). The fast estimate would cross that
 * ~50 frames earlier per utterance and erode the learned filter;
 * vnr_hold_adapt_vnr() keeps the leakage on the EMA's timing.
 */

#define VNR_HOLD_ATTACK_ALPHA   0.5f    /* weight of the previous estimate while the VNR rises */
#define VNR_HOLD_RELEASE_ALPHA  0.97f   /* ... and while it falls: IC_INIT_VNR_PRED_ALPHA */

typedef struct {
    float value;
} vnr_hold_t;

void vnr_hold_init(vnr_hold_t *hold, float initial);

/**
 * \param vnr   this frame's unsmoothed VNR inference output, 0..1
 * \return      the smoothed estimate
 */
float vnr_hold_update(vnr_hold_t *hold, float vnr);

/**
 * VNR for ic_adapt(): the fast estimate, kept at or below threshold_high
 * until the fwk_voice EMA is above it as well.
 *
 * \param hold_vnr        vnr_hold_update() output
 * \param ema_vnr         the fwk_voice smoothed input VNR
 * \param threshold_high  the IC's input_vnr_threshold_high
 */
float vnr_hold_adapt_vnr(float hold_vnr, float ema_vnr, float threshold_high);
