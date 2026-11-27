#include <assert.h>
#include <string.h>
#include "stats_servicer/stats_servicer.h"
#include "stats_servicer/stats_cmds.h"
#include "audio/audio_quality.h"
#define xassert assert

/* Real command-map, callbacks, initialization and shared validation helpers;
 * only the measurement providers and device-control transport are stubbed. */
#include "servicer_helpers.inc"

void stage_stats_read_times_us(uint16_t last[STAGE_STATS_COUNT], uint16_t max[STAGE_STATS_COUNT])
{
    for (unsigned i = 0; i < STAGE_STATS_COUNT; ++i) {
        last[i] = (uint16_t)i;
        max[i] = (uint16_t)(100 + i);
    }
}
void stage_stats_read_stack_free(uint16_t free_words[STAGE_STATS_COUNT])
{
    for (unsigned i = 0; i < STAGE_STATS_COUNT; ++i) free_words[i] = (uint16_t)(200 + i);
}
void stage_stats_read_heap(uint32_t heap[4])
{
    for (unsigned i = 0; i < 4; ++i) heap[i] = 0x12345600u + i;
}
void audio_quality_read(uint8_t payload[AUDIO_QUALITY_PAYLOAD_BYTES])
{
    for (unsigned i = 0; i < AUDIO_QUALITY_PAYLOAD_BYTES; ++i) payload[i] = (uint8_t)i;
}

#include "stats_dispatch.inc"

int main(void)
{
    assert(STATS_SERVICER_CMD_STAGE_TIMES == 0 && STATS_SERVICER_CMD_STACK_FREE == 1);
    assert(STATS_SERVICER_CMD_HEAP_FREE == 2 && STATS_SERVICER_CMD_AUDIO_QUALITY == 3);
    assert(STAGE_STATS_COUNT == 7 && NUM_STATS_SERVICER_CMDS == 4);
    servicer_t servicer;
    stats_servicer_init(&servicer);
    uint8_t payload[40];
    const size_t lengths[] = {29, 15, 17, 33};
    for (unsigned cmd = 0; cmd < 4; ++cmd) {
        memset(payload, 0xaa, sizeof(payload));
        assert(stats_servicer_read_cmd(250, 0x80 | cmd, payload, lengths[cmd], &servicer) == CONTROL_SUCCESS);
        assert(payload[0] == CONTROL_SUCCESS && payload[lengths[cmd]] == 0xaa);
        assert(stats_servicer_write_cmd(250, cmd, payload, lengths[cmd] - 1, &servicer) == CONTROL_BAD_COMMAND);
        if (cmd == 0) {
            for (unsigned i = 0; i < 7; ++i) {
                assert(payload[1 + i * 2] == i && payload[2 + i * 2] == 0);
                assert(payload[15 + i * 2] == 100 + i && payload[16 + i * 2] == 0);
            }
        } else if (cmd == 1) {
            for (unsigned i = 0; i < 7; ++i) assert(payload[1 + i * 2] == 200 + i && payload[2 + i * 2] == 0);
        } else if (cmd == 2) {
            for (unsigned i = 0; i < 4; ++i) {
                assert(payload[1 + i * 4] == i && payload[2 + i * 4] == 0x56);
                assert(payload[3 + i * 4] == 0x34 && payload[4 + i * 4] == 0x12);
            }
        } else {
            for (unsigned i = 0; i < 32; ++i) assert(payload[i + 1] == i);
        }
    }
    const size_t bad_lengths[] = {0, 1, 32, 34};
    for (unsigned i = 0; i < sizeof(bad_lengths) / sizeof(bad_lengths[0]); ++i) {
        assert(stats_servicer_read_cmd(250, 0x83, payload, bad_lengths[i], &servicer) == SERVICER_WRONG_COMMAND_LEN);
        assert(payload[0] == SERVICER_WRONG_COMMAND_LEN);
    }
    assert(stats_servicer_read_cmd(250, 0x84, payload, 33, &servicer) == SERVICER_WRONG_COMMAND_ID);
    assert(payload[0] == SERVICER_WRONG_COMMAND_ID);
    return 0;
}
