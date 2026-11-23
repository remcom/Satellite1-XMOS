#include <assert.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "audio/audio_quality.h"
#include "rtos_osal.h"

static rtos_osal_mutex_t *publication_lock;

rtos_osal_status_t rtos_osal_mutex_create(rtos_osal_mutex_t *lock, char *name, int recursive)
{
    (void)name;
    assert(!recursive);
    publication_lock = lock;
    return pthread_mutex_init(lock, NULL) == 0 ? RTOS_OSAL_SUCCESS : RTOS_OSAL_ERROR;
}

rtos_osal_status_t rtos_osal_mutex_get(rtos_osal_mutex_t *lock, unsigned timeout)
{
    assert(timeout == RTOS_OSAL_NO_WAIT || timeout == RTOS_OSAL_WAIT_FOREVER);
    const int status = timeout == RTOS_OSAL_NO_WAIT
        ? pthread_mutex_trylock(lock) : pthread_mutex_lock(lock);
    return status == 0 ? RTOS_OSAL_SUCCESS : RTOS_OSAL_TIMEOUT;
}

rtos_osal_status_t rtos_osal_mutex_put(rtos_osal_mutex_t *lock)
{
    return pthread_mutex_unlock(lock) == 0 ? RTOS_OSAL_SUCCESS : RTOS_OSAL_ERROR;
}

static uint32_t u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void test_energy(void)
{
    const float_s32_t zero = {0, INT_MIN};
    const float_s32_t one = {1, 0};
    assert(audio_quality_erle_cdb(zero, zero) == AUDIO_QUALITY_ERLE_UNKNOWN);
    assert(audio_quality_erle_cdb(one, zero) == 9600);
    assert(audio_quality_erle_cdb(zero, one) == -9600);
    assert(audio_quality_erle_cdb((float_s32_t){-1, 0}, one) == AUDIO_QUALITY_ERLE_UNKNOWN);
    assert(audio_quality_erle_cdb(one, (float_s32_t){-1, 0}) == AUDIO_QUALITY_ERLE_UNKNOWN);
    assert(audio_quality_erle_cdb(one, one) == 0);
    assert(audio_quality_erle_cdb((float_s32_t){100, 0}, one) == 2000);
    assert(audio_quality_erle_cdb(one, (float_s32_t){100, 0}) == -2000);
    assert(audio_quality_erle_cdb((float_s32_t){1, -1000}, (float_s32_t){1, -1010}) == 3010);
    assert(audio_quality_erle_cdb((float_s32_t){1, 1000}, (float_s32_t){1, 1010}) == -3010);
    assert(audio_quality_erle_cdb((float_s32_t){1, INT_MAX}, (float_s32_t){1, INT_MAX - 1}) == 301);
    assert(audio_quality_erle_cdb((float_s32_t){1, INT_MAX}, (float_s32_t){1, INT_MIN}) == 9600);
    assert(audio_quality_erle_cdb((float_s32_t){1, INT_MIN}, (float_s32_t){1, INT_MAX}) == -9600);
    assert(audio_quality_erle_cdb((float_s32_t){INT_MAX, 0}, one) == 9332);
    assert(audio_quality_erle_cdb(one, (float_s32_t){INT_MAX, 0}) == -9332);
    /* Equivalent BFP representations and independently evaluated ratios. */
    for (int i = 1; i < 100; ++i) {
        const int16_t expected = (int16_t)lround(1000.0 * log10((double)i / 37.0));
        assert(audio_quality_erle_cdb((float_s32_t){i, -31}, (float_s32_t){37, -31}) == expected);
        assert(audio_quality_erle_cdb((float_s32_t){i * 2, -32}, (float_s32_t){37, -31}) == expected);
    }
}

static void test_layout(void)
{
    audio_quality_snapshot_t s = {0};
    s.t1.aec_channels = 2;
    s.output_channels = 2;
    s.flags = 3;
    s.completed_frames = 0x12345678;
    s.t1.ref_drops = (audio_quality_ref_drops_t){0x11223344, 0x55667788};
    s.t1.input_energy[0] = (float_s32_t){100, 0};
    s.t1.error_energy[0] = (float_s32_t){1, 0};
    s.t1.input_energy[1] = (float_s32_t){1, 0};
    s.t1.error_energy[1] = (float_s32_t){100, 0};
    s.clips[0] = UINT32_MAX;
    s.clips[1] = 0x01020304;
    const uint8_t expected[32] = {
        1, 2, 2, 3, 0x78, 0x56, 0x34, 0x12,
        0x44, 0x33, 0x22, 0x11, 0x88, 0x77, 0x66, 0x55,
        0xd0, 0x07, 0x30, 0xf8, 0, 0x80, 0, 0x80,
        0xff, 0xff, 0xff, 0xff, 4, 3, 2, 1,
    };
    uint8_t guarded[AUDIO_QUALITY_PAYLOAD_BYTES + 2];
    memset(guarded, 0xab, sizeof(guarded));
    audio_quality_encode(guarded + 1, &s);
    assert(guarded[0] == 0xab && guarded[sizeof(guarded) - 1] == 0xab);
    assert(memcmp(guarded + 1, expected, sizeof(expected)) == 0);
    /* Four raw AEC channels, in addition to the two-channel layout above. */
    s.t1.aec_channels = 4;
    s.t1.input_energy[2] = s.t1.input_energy[3] = (float_s32_t){1, 0};
    audio_quality_encode(guarded + 1, &s);
    assert(guarded[2] == 4);
    assert(guarded[21] == 0x80 && guarded[22] == 0x25); /* +9600 */
    assert(guarded[23] == 0x80 && guarded[24] == 0x25);
    /* Bypass/missing stages: no fake zero-dB cancellation. */
    memset(&s, 0, sizeof(s));
    audio_quality_encode(guarded + 1, &s);
    for (unsigned i = 0; i < 4; ++i) {
        assert(guarded[17 + i * 2] == 0 && guarded[18 + i * 2] == 0x80);
    }
    assert(guarded[2] == 0 && guarded[3] == 0 && guarded[4] == 0);
}

static void test_clips_and_drops(void)
{
    const int32_t t = AUDIO_QUALITY_CLIP_THRESHOLD;
    const int32_t samples[][2] = {
        {t - 1, -t + 1}, {t, -t}, {t + 1, -t - 1},
        {INT32_MAX, INT32_MIN}, {0, -1}, {-t, t},
    };
    uint32_t clips[2] = {0};
    audio_quality_count_clips(clips, &samples[0][0], 6);
    assert(clips[0] == 4 && clips[1] == 4);
    audio_quality_count_clips(clips, &samples[0][0], 0);
    assert(clips[0] == 4 && clips[1] == 4);
    clips[0] = UINT32_MAX - 1;
    clips[1] = UINT32_MAX;
    audio_quality_count_clips(clips, &samples[0][0], 6);
    assert(clips[0] == UINT32_MAX && clips[1] == UINT32_MAX);

    audio_quality_ref_drops_t drops = {0};
    /* Queue full alone is not a drop: consumer may drain before discard. */
    audio_quality_account_reference_drops(&drops, 0, 0);
    assert(drops.oldest == 0 && drops.newest == 0);
    audio_quality_account_reference_drops(&drops, 1, 0);
    assert(drops.oldest == 1 && drops.newest == 0);
    audio_quality_account_reference_drops(&drops, 0, 1);
    assert(drops.oldest == 1 && drops.newest == 1);
    audio_quality_account_reference_drops(&drops, 1, 1);
    assert(drops.oldest == 2 && drops.newest == 2);
    drops.oldest = UINT32_MAX - 1;
    drops.newest = UINT32_MAX;
    audio_quality_account_reference_drops(&drops, 1, 1);
    audio_quality_account_reference_drops(&drops, 1, 1);
    assert(drops.oldest == UINT32_MAX && drops.newest == UINT32_MAX);
}

static audio_quality_t1_report_t report_for(uint32_t frame)
{
    audio_quality_t1_report_t report = {0};
    report.aec_channels = 2;
    report.ref_drops = (audio_quality_ref_drops_t){frame, frame * 2};
    report.input_energy[0] = report.input_energy[1] = (float_s32_t){100, 0};
    report.error_energy[0] = report.error_energy[1] = (float_s32_t){1, 0};
    return report;
}

static void *publish_many(void *unused)
{
    (void)unused;
    const int32_t samples[2] = {INT32_MAX, INT32_MIN};
    for (uint32_t frame = 4; frame < 10004; ++frame) {
        audio_quality_record_output(samples, 1);
        const audio_quality_t1_report_t report = report_for(frame);
        audio_quality_publish_frame(&report);
    }
    return NULL;
}

static void check_snapshot(const uint8_t *p)
{
    const uint32_t frame = u32(p + 4);
    assert(p[0] == 1 && p[1] == 2 && p[2] == 2 && p[3] == 3);
    assert(u32(p + 8) == frame && u32(p + 12) == frame * 2);
    assert(u32(p + 24) == frame && u32(p + 28) == frame);
    assert(p[16] == 0xd0 && p[17] == 7 && p[18] == 0xd0 && p[19] == 7);
}

static void test_publication(void)
{
    audio_quality_init(2, 1);
    uint8_t before[32], after[32];
    audio_quality_read(before);
    assert(before[1] == 0 && before[2] == 2 && before[3] == 1 && u32(before + 4) == 0);
    const int32_t samples[2] = {INT32_MAX, INT32_MIN};
    audio_quality_record_output(samples, 1);
    audio_quality_t1_report_t report = report_for(1);
    audio_quality_publish_frame(&report);
    audio_quality_read(before);
    check_snapshot(before);
    /* Reader holds the publication lock. Output still advances and counts;
     * this call would deadlock immediately if publication waited for it. */
    assert(pthread_mutex_lock(publication_lock) == 0);
    audio_quality_record_output(samples, 1);
    report = report_for(2);
    audio_quality_publish_frame(&report);
    assert(pthread_mutex_unlock(publication_lock) == 0);
    audio_quality_read(after);
    assert(memcmp(before, after, sizeof(before)) == 0);
    audio_quality_record_output(samples, 1);
    report = report_for(3);
    audio_quality_publish_frame(&report);
    audio_quality_read(after);
    check_snapshot(after); /* includes clips from the unpublished frame */
    audio_quality_read(before);
    assert(memcmp(before, after, sizeof(before)) == 0); /* no read/reset */

    pthread_t publisher;
    assert(pthread_create(&publisher, NULL, publish_many, NULL) == 0);
    for (int i = 0; i < 10000; ++i) {
        audio_quality_read(after);
        check_snapshot(after);
    }
    assert(pthread_join(publisher, NULL) == 0);
    assert(pthread_mutex_destroy(publication_lock) == 0);
    audio_quality_init(0, 0);
    audio_quality_read(after);
    assert(after[1] == 0 && after[2] == 0 && after[3] == 0 && u32(after + 24) == 0);
    assert(pthread_mutex_destroy(publication_lock) == 0);
}

int main(void)
{
    test_energy();
    test_layout();
    test_clips_and_drops();
    test_publication();
    puts("audio quality tests passed");
    return 0;
}
