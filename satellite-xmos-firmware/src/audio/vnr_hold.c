#include "audio/vnr_hold.h"

void vnr_hold_init(vnr_hold_t *hold, float initial)
{
    hold->value = initial;
}

float vnr_hold_update(vnr_hold_t *hold, float vnr)
{
    const float alpha = (vnr > hold->value) ? VNR_HOLD_ATTACK_ALPHA : VNR_HOLD_RELEASE_ALPHA;
    hold->value = alpha * hold->value + (1.0f - alpha) * vnr;
    return hold->value;
}
