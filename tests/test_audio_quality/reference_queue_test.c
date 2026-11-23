/* Compile the actual speaker_pipeline_output() body from main.c with scripted
 * queue outcomes. DSP and I2S are stubs; queue/drop decisions are firmware C. */
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "audio/audio_quality.h"

#define ON_TILE(tile) 1
#define SPEAKER_PIPELINE_TILE_NO 1
#define appconfAUDIO_SPK_PIPELINE_FRAME_ADVANCE 4
#define appconfAUDIO_PIPELINE_FRAME_ADVANCE 4
#define appconfAUDIO_PIPELINE_REF_CHANNELS 2
#define appconfAUDIO_PIPELINE_AEC_REF_CHANNELS 2
#define appconfAUDIO_PIPELINE_SAMPLE_RATE 16000
#define appconfI2S_AUDIO_SAMPLE_RATE 16000
#define STAGE_STATS_T1_SPK_SRC 2
#define portMAX_DELAY UINT32_MAX
#define RTOS_OSAL_SUCCESS 0
#define xassert assert

typedef struct {
    int32_t samples[4][2];
    audio_quality_ref_drops_t drops;
} ref_frame_t;
typedef struct { int unused; } voice_src_ds3_t;
static void *i2s_ctx;
static void *ref_input_queue;
static int sends[2];
static unsigned send_index;
static int receive_result;
static unsigned receives;
static ref_frame_t queued;

static uint32_t stage_stats_start(void) { return 0; }
static void stage_stats_end(unsigned stage, uint32_t start) { (void)stage; (void)start; }
static int32_t voice_src_ds3(voice_src_ds3_t *state, const int32_t *in)
{ (void)state; return in[0]; }
static void rtos_i2s_tx_1(void *ctx, int32_t *samples, size_t count, unsigned timeout)
{
    (void)ctx;
    assert(count == 4 && timeout == portMAX_DELAY);
    assert(samples[0] == 1 && samples[1] == 5);
}
static int rtos_osal_queue_send(void *queue, const void *item, unsigned timeout)
{
    (void)queue;
    assert(timeout == 0 && send_index < 2);
    const int result = sends[send_index++];
    if (result == RTOS_OSAL_SUCCESS) queued = *(const ref_frame_t *)item;
    return result;
}
static int rtos_osal_queue_receive(void *queue, void *item, unsigned timeout)
{
    (void)queue;
    assert(timeout == 0);
    ++receives;
    memset(item, 0, sizeof(ref_frame_t));
    return receive_result;
}

#include "speaker_output.inc"

static void enqueue(int initial, int removal, int retry)
{
    sends[0] = initial;
    sends[1] = retry;
    send_index = 0;
    receive_result = removal;
    receives = 0;
    const int32_t samples[] = {1, 2, 3, 4, 5, 6, 7, 8};
    speaker_pipeline_output(NULL, samples, 4);
    assert(send_index == (initial == RTOS_OSAL_SUCCESS ? 1u : 2u));
    assert(receives == (initial == RTOS_OSAL_SUCCESS ? 0u : 1u));
}

int main(void)
{
    enqueue(0, 0, 0); /* initial enqueue succeeds */
    assert(queued.drops.oldest == 0 && queued.drops.newest == 0);
    assert(queued.samples[0][0] == 1 && queued.samples[0][1] == 5);
    enqueue(1, 0, 0); /* full queue, oldest removed, retry succeeds */
    assert(queued.drops.oldest == 1 && queued.drops.newest == 0);
    enqueue(1, 1, 0); /* consumer raced with producer: no actual removal */
    assert(queued.drops.oldest == 1 && queued.drops.newest == 0);
    enqueue(1, 0, 1); /* oldest and newest lost; no frame to carry totals yet */
    enqueue(0, 0, 0);
    assert(queued.drops.oldest == 2 && queued.drops.newest == 1);
    enqueue(1, 1, 1); /* only newest lost */
    enqueue(0, 0, 0);
    assert(queued.drops.oldest == 2 && queued.drops.newest == 2);
    return 0;
}
