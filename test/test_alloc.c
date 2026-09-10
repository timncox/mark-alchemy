/*
 * Mark on the SDRAM bump allocator, natively.
 *
 * 1. mark_create() lands in the firmware's pool (VERSIO_POOL_BYTES) at the
 *    firmware's capacity (MARK_MAX_SECONDS) on the FIRST rung of its
 *    allocation ladder, with nothing failed. This is the only guard against
 *    the ladder leaking on the allocator's no-op free: on USB power a
 *    fault_forever at boot is invisible.
 * 2. Unclocked, the first loop is free length and becomes the bar: a
 *    two-second recording at 120 BPM finalizes as exactly one bar of 96,000
 *    frames and the track plays.
 * 3. The loop is audible: playback reproduces what was recorded.
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/versio_alloc.h"
#include "mark_core.h"

#define BLK 128

static float fake_bpm(void) { return 120.0f; }

static long get_int(mark_t *m, const char *k)
{
    char buf[64];
    assert(mark_get_param(m, k, buf, sizeof buf) > 0);
    return atol(buf);
}

int main(void)
{
    void *pool = malloc(VERSIO_POOL_BYTES);
    assert(pool);
    versio_alloc_init(pool, VERSIO_POOL_BYTES);

    host_api_v1_t host;
    memset(&host, 0, sizeof host);
    host.api_version      = 1;
    host.sample_rate      = MARK_SR;
    host.frames_per_block = BLK;
    host.get_bpm          = fake_bpm;

    mark_t *m = mark_create(&host);
    assert(m);
    assert(!versio_alloc_failed());
    const long secs = get_int(m, "track_seconds");
    printf("pool %zu B, engine took %zu B (%.1f%%), %ld s per track\n",
           versio_alloc_capacity(), versio_alloc_used(),
           100.0 * (double)versio_alloc_used() / (double)versio_alloc_capacity(), secs);
    assert(secs == MARK_MAX_SECONDS);
    /* First rung: exactly (tracks + undo) buffers of the full capacity. */
    assert(versio_alloc_used() >= (size_t)(MARK_TRACKS + 1) * MARK_MAX_SECONDS * MARK_SR * 4u);
    /* Nothing else draws from the pool, so the margin only has to catch the
     * engine struct growing past it: 128 KB of a 62 MiB pool. */
    assert(versio_alloc_capacity() - versio_alloc_used() >= 128u * 1024u);

    /* 2. unclocked first loop: record two seconds of a 220 Hz sine */
    mark_set_param(m, "monitor",  "0");   /* loops only at the output */
    mark_set_param(m, "quantize", "1");
    mark_set_param(m, "rec_grid", "0");
    mark_set_param(m, "bpm_override", "120");

    int16_t io[BLK * 2];
    double  ph = 0.0;
    mark_set_param(m, "t1_btn", "1");
    assert(get_int(m, "t1_state") == MK_REC);   /* immediate: nothing playing, no clock */
    const long rec_frames = 2 * MARK_SR;
    for (long done = 0; done < rec_frames; done += BLK)
    {
        for (int i = 0; i < BLK; i++)
        {
            int16_t v = (int16_t)(0.5 * 32767.0 * sin(ph));
            ph += 2.0 * M_PI * 220.0 / MARK_SR;
            io[2 * i] = io[2 * i + 1] = v;
        }
        mark_process(m, io, io, BLK);
    }
    mark_set_param(m, "t1_btn", "1");
    /* 2 s at 120 BPM = one bar = 96,000 frames: rounds to itself, finalizes
     * at once, and the track is playing. */
    assert(get_int(m, "t1_state") == MK_PLAY);
    const long len = get_int(m, "t1_len");
    printf("loop length %ld frames (%.3f s)\n", len, (double)len / MARK_SR);
    assert(len == 2 * MARK_SR);

    /* 3. playback is the recording: silence in, sine out */
    double energy = 0.0;
    for (int blk = 0; blk < 200; blk++)
    {
        memset(io, 0, sizeof io);
        mark_process(m, io, io, BLK);
        for (int i = 0; i < BLK; i++) energy += (double)io[2 * i] * io[2 * i];
    }
    energy = sqrt(energy / (200.0 * BLK)) / 32767.0;
    printf("playback rms %.3f\n", energy);
    assert(energy > 0.2 && energy < 0.5);   /* a 0.5-amplitude sine is ~0.35 */

    /* stop, then clear, both immediate on a stopped track */
    mark_set_param(m, "t1_stop", "1");
    assert(get_int(m, "t1_state") == MK_STOP);
    mark_set_param(m, "t1_clear", "1");
    assert(get_int(m, "t1_state") == MK_EMPTY);

    puts("test_alloc: ok");
    free(pool);
    return 0;
}
