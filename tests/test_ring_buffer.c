/*
 * test_ring_buffer.c — Standalone C unit tests for SharedRingBuffer.
 *
 * Compile:
 *   cc -std=c11 -Wall -Wextra -Werror -I MacAudioDriver \
 *      -DkSHM_Name='"/macaudio_test_rb"' \
 *      -o tests/test_ring_buffer \
 *      tests/test_ring_buffer.c MacAudioDriver/SharedRingBuffer.c
 *
 * Run:
 *   ./tests/test_ring_buffer
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "SharedRingBuffer.h"

static int tests_run = 0;
static int tests_passed = 0;

#define RUN_TEST(fn) do { \
    printf("  %-50s", #fn); \
    tests_run++; \
    fn(); \
    tests_passed++; \
    printf("PASS\n"); \
} while (0)

/* Helper: create a fresh writer handle, cleaning up any prior shm. */
static SharedRingBuffer* fresh_writer(void) {
    SharedRingBuffer_Destroy();
    SharedRingBuffer* w = SharedRingBuffer_CreateOrOpen(1);
    assert(w != NULL);
    return w;
}

/* ---------- Write/Read Roundtrip ---------- */

static void test_write_read_simple(void) {
    SharedRingBuffer* w = fresh_writer();
    SharedRingBuffer* r = SharedRingBuffer_CreateOrOpen(0);
    assert(r != NULL);

    float src[4] = {0.1f, 0.2f, 0.3f, 0.4f};  /* 2 frames, 2 channels */
    SharedRingBuffer_Write(w, src, 2);

    float dst[4] = {0};
    uint64_t read = SharedRingBuffer_Read(r, dst, 2);
    assert(read == 2);
    for (int i = 0; i < 4; i++) {
        assert(fabsf(dst[i] - src[i]) < 1e-6f);
    }

    SharedRingBuffer_Close(r);
    SharedRingBuffer_Close(w);
    SharedRingBuffer_Destroy();
}

static void test_write_read_large_block(void) {
    SharedRingBuffer* w = fresh_writer();
    SharedRingBuffer* r = SharedRingBuffer_CreateOrOpen(0);
    assert(r != NULL);

    uint32_t frames = 1024;
    uint32_t samples = frames * kNumChannels;
    float* src = calloc(samples, sizeof(float));
    float* dst = calloc(samples, sizeof(float));
    assert(src && dst);

    for (uint32_t i = 0; i < samples; i++) src[i] = (float)i / (float)samples;
    SharedRingBuffer_Write(w, src, frames);

    uint64_t read = SharedRingBuffer_Read(r, dst, frames);
    assert(read == frames);
    for (uint32_t i = 0; i < samples; i++) {
        assert(fabsf(dst[i] - src[i]) < 1e-6f);
    }

    free(src);
    free(dst);
    SharedRingBuffer_Close(r);
    SharedRingBuffer_Close(w);
    SharedRingBuffer_Destroy();
}

static void test_multiple_writes_single_read(void) {
    SharedRingBuffer* w = fresh_writer();
    SharedRingBuffer* r = SharedRingBuffer_CreateOrOpen(0);
    assert(r != NULL);

    float a[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    float b[4] = {5.0f, 6.0f, 7.0f, 8.0f};
    SharedRingBuffer_Write(w, a, 2);
    SharedRingBuffer_Write(w, b, 2);

    float dst[8] = {0};
    uint64_t read = SharedRingBuffer_Read(r, dst, 4);
    assert(read == 4);
    assert(fabsf(dst[0] - 1.0f) < 1e-6f);
    assert(fabsf(dst[4] - 5.0f) < 1e-6f);

    SharedRingBuffer_Close(r);
    SharedRingBuffer_Close(w);
    SharedRingBuffer_Destroy();
}

static void test_write_read_preserves_channel_interleaving(void) {
    SharedRingBuffer* w = fresh_writer();
    SharedRingBuffer* r = SharedRingBuffer_CreateOrOpen(0);
    assert(r != NULL);

    /* 3 frames of stereo: L0 R0 L1 R1 L2 R2 */
    float src[6] = {-1.0f, 1.0f, -0.5f, 0.5f, 0.0f, 0.0f};
    SharedRingBuffer_Write(w, src, 3);

    float dst[6] = {0};
    uint64_t read = SharedRingBuffer_Read(r, dst, 3);
    assert(read == 3);
    for (int i = 0; i < 6; i++) {
        assert(fabsf(dst[i] - src[i]) < 1e-6f);
    }

    SharedRingBuffer_Close(r);
    SharedRingBuffer_Close(w);
    SharedRingBuffer_Destroy();
}

/* ---------- Wraparound ---------- */

static void test_write_wraps_around_buffer(void) {
    SharedRingBuffer* w = fresh_writer();
    SharedRingBuffer* r = SharedRingBuffer_CreateOrOpen(0);
    assert(r != NULL);

    /* Fill most of the buffer, read it, then write across the boundary. */
    uint32_t almost_full = kRingBufferFrames - 100;
    float* fill = calloc(almost_full * kNumChannels, sizeof(float));
    assert(fill);
    for (uint32_t i = 0; i < almost_full * kNumChannels; i++) fill[i] = 0.1f;
    SharedRingBuffer_Write(w, fill, almost_full);

    float* sink = calloc(almost_full * kNumChannels, sizeof(float));
    assert(sink);
    SharedRingBuffer_Read(r, sink, almost_full);
    free(sink);
    free(fill);

    /* Now write 200 frames — should wrap around the end. */
    uint32_t wrap_frames = 200;
    float* wrap_src = calloc(wrap_frames * kNumChannels, sizeof(float));
    float* wrap_dst = calloc(wrap_frames * kNumChannels, sizeof(float));
    assert(wrap_src && wrap_dst);
    for (uint32_t i = 0; i < wrap_frames * kNumChannels; i++) wrap_src[i] = (float)i;
    SharedRingBuffer_Write(w, wrap_src, wrap_frames);

    uint64_t read = SharedRingBuffer_Read(r, wrap_dst, wrap_frames);
    assert(read == wrap_frames);
    for (uint32_t i = 0; i < wrap_frames * kNumChannels; i++) {
        assert(fabsf(wrap_dst[i] - wrap_src[i]) < 1e-6f);
    }

    free(wrap_src);
    free(wrap_dst);
    SharedRingBuffer_Close(r);
    SharedRingBuffer_Close(w);
    SharedRingBuffer_Destroy();
}

static void test_write_exactly_fills_buffer(void) {
    SharedRingBuffer* w = fresh_writer();
    SharedRingBuffer* r = SharedRingBuffer_CreateOrOpen(0);
    assert(r != NULL);

    float* src = calloc(kRingBufferFrames * kNumChannels, sizeof(float));
    float* dst = calloc(kRingBufferFrames * kNumChannels, sizeof(float));
    assert(src && dst);
    for (uint32_t i = 0; i < kRingBufferFrames * kNumChannels; i++) src[i] = (float)i;

    SharedRingBuffer_Write(w, src, kRingBufferFrames);
    uint64_t read = SharedRingBuffer_Read(r, dst, kRingBufferFrames);
    assert(read == kRingBufferFrames);
    for (uint32_t i = 0; i < kRingBufferFrames * kNumChannels; i++) {
        assert(fabsf(dst[i] - src[i]) < 1e-6f);
    }

    free(src);
    free(dst);
    SharedRingBuffer_Close(r);
    SharedRingBuffer_Close(w);
    SharedRingBuffer_Destroy();
}

static void test_multiple_wraparounds(void) {
    SharedRingBuffer* w = fresh_writer();
    SharedRingBuffer* r = SharedRingBuffer_CreateOrOpen(0);
    assert(r != NULL);

    /* Write and read 3x the buffer size in chunks. */
    uint32_t chunk = 1000;
    uint32_t total_frames = kRingBufferFrames * 3;
    float* src = calloc(chunk * kNumChannels, sizeof(float));
    float* dst = calloc(chunk * kNumChannels, sizeof(float));
    assert(src && dst);

    uint32_t frames_processed = 0;
    while (frames_processed < total_frames) {
        for (uint32_t i = 0; i < chunk * kNumChannels; i++)
            src[i] = (float)(frames_processed * kNumChannels + i);
        SharedRingBuffer_Write(w, src, chunk);

        uint64_t read = SharedRingBuffer_Read(r, dst, chunk);
        assert(read == chunk);
        for (uint32_t i = 0; i < chunk * kNumChannels; i++) {
            assert(fabsf(dst[i] - src[i]) < 1e-6f);
        }
        frames_processed += chunk;
    }

    free(src);
    free(dst);
    SharedRingBuffer_Close(r);
    SharedRingBuffer_Close(w);
    SharedRingBuffer_Destroy();
}

/* ---------- Head Tracking ---------- */

static void test_write_head_advances(void) {
    SharedRingBuffer* w = fresh_writer();
    assert(SharedRingBuffer_GetWriteHead(w) == 0);

    float buf[4] = {0};
    SharedRingBuffer_Write(w, buf, 2);
    assert(SharedRingBuffer_GetWriteHead(w) == 2);

    SharedRingBuffer_Write(w, buf, 2);
    assert(SharedRingBuffer_GetWriteHead(w) == 4);

    SharedRingBuffer_Close(w);
    SharedRingBuffer_Destroy();
}

static void test_read_head_advances(void) {
    SharedRingBuffer* w = fresh_writer();
    SharedRingBuffer* r = SharedRingBuffer_CreateOrOpen(0);
    assert(r != NULL);
    assert(SharedRingBuffer_GetReadHead(r) == 0);

    float src[8] = {0};
    SharedRingBuffer_Write(w, src, 4);

    float dst[4] = {0};
    SharedRingBuffer_Read(r, dst, 2);
    assert(SharedRingBuffer_GetReadHead(r) == 2);

    SharedRingBuffer_Read(r, dst, 2);
    assert(SharedRingBuffer_GetReadHead(r) == 4);

    SharedRingBuffer_Close(r);
    SharedRingBuffer_Close(w);
    SharedRingBuffer_Destroy();
}

static void test_heads_monotonically_increase(void) {
    SharedRingBuffer* w = fresh_writer();
    SharedRingBuffer* r = SharedRingBuffer_CreateOrOpen(0);
    assert(r != NULL);

    float buf[2] = {0};
    uint64_t prev_w = 0, prev_r = 0;
    for (int i = 0; i < 100; i++) {
        SharedRingBuffer_Write(w, buf, 1);
        uint64_t wh = SharedRingBuffer_GetWriteHead(w);
        assert(wh > prev_w);
        prev_w = wh;

        float out[2] = {0};
        SharedRingBuffer_Read(r, out, 1);
        uint64_t rh = SharedRingBuffer_GetReadHead(r);
        assert(rh > prev_r);
        prev_r = rh;
    }

    SharedRingBuffer_Close(r);
    SharedRingBuffer_Close(w);
    SharedRingBuffer_Destroy();
}

/* ---------- Active / SampleRate ---------- */

static void test_default_sample_rate(void) {
    SharedRingBuffer* w = fresh_writer();
    SharedRingBuffer* r = SharedRingBuffer_CreateOrOpen(0);
    assert(r != NULL);
    assert(SharedRingBuffer_GetSampleRate(r) == 48000);
    SharedRingBuffer_Close(r);
    SharedRingBuffer_Close(w);
    SharedRingBuffer_Destroy();
}

static void test_set_sample_rate(void) {
    SharedRingBuffer* w = fresh_writer();
    SharedRingBuffer* r = SharedRingBuffer_CreateOrOpen(0);
    assert(r != NULL);

    SharedRingBuffer_SetSampleRate(w, 44100);
    assert(SharedRingBuffer_GetSampleRate(r) == 44100);

    SharedRingBuffer_SetSampleRate(w, 96000);
    assert(SharedRingBuffer_GetSampleRate(r) == 96000);

    SharedRingBuffer_Close(r);
    SharedRingBuffer_Close(w);
    SharedRingBuffer_Destroy();
}

static void test_default_active_is_zero(void) {
    SharedRingBuffer* w = fresh_writer();
    SharedRingBuffer* r = SharedRingBuffer_CreateOrOpen(0);
    assert(r != NULL);
    assert(SharedRingBuffer_GetActive(r) == 0);
    SharedRingBuffer_Close(r);
    SharedRingBuffer_Close(w);
    SharedRingBuffer_Destroy();
}

static void test_set_active(void) {
    SharedRingBuffer* w = fresh_writer();
    SharedRingBuffer* r = SharedRingBuffer_CreateOrOpen(0);
    assert(r != NULL);

    SharedRingBuffer_SetActive(w, 1);
    assert(SharedRingBuffer_GetActive(r) == 1);

    SharedRingBuffer_SetActive(w, 0);
    assert(SharedRingBuffer_GetActive(r) == 0);

    SharedRingBuffer_Close(r);
    SharedRingBuffer_Close(w);
    SharedRingBuffer_Destroy();
}

/* ---------- Edge Cases ---------- */

static void test_zero_length_write(void) {
    SharedRingBuffer* w = fresh_writer();
    float buf[2] = {1.0f, 2.0f};
    SharedRingBuffer_Write(w, buf, 0);
    assert(SharedRingBuffer_GetWriteHead(w) == 0);
    SharedRingBuffer_Close(w);
    SharedRingBuffer_Destroy();
}

static void test_read_more_than_available(void) {
    SharedRingBuffer* w = fresh_writer();
    SharedRingBuffer* r = SharedRingBuffer_CreateOrOpen(0);
    assert(r != NULL);

    float src[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    SharedRingBuffer_Write(w, src, 2);

    float dst[20] = {0};
    uint64_t read = SharedRingBuffer_Read(r, dst, 10);
    assert(read == 2);  /* Only 2 frames available */
    assert(fabsf(dst[0] - 1.0f) < 1e-6f);

    SharedRingBuffer_Close(r);
    SharedRingBuffer_Close(w);
    SharedRingBuffer_Destroy();
}

static void test_read_empty_buffer(void) {
    SharedRingBuffer* w = fresh_writer();
    SharedRingBuffer* r = SharedRingBuffer_CreateOrOpen(0);
    assert(r != NULL);

    float dst[4] = {0};
    uint64_t read = SharedRingBuffer_Read(r, dst, 2);
    assert(read == 0);

    SharedRingBuffer_Close(r);
    SharedRingBuffer_Close(w);
    SharedRingBuffer_Destroy();
}

/* ---------- NULL Safety ---------- */

static void test_null_write(void) {
    /* Should not crash. */
    SharedRingBuffer_Write(NULL, NULL, 10);
    float buf[2] = {0};
    SharedRingBuffer_Write(NULL, buf, 1);
}

static void test_null_read(void) {
    /* Should not crash, returns 0. */
    uint64_t r1 = SharedRingBuffer_Read(NULL, NULL, 10);
    assert(r1 == 0);
    float buf[2] = {0};
    uint64_t r2 = SharedRingBuffer_Read(NULL, buf, 1);
    assert(r2 == 0);
    assert(SharedRingBuffer_GetActive(NULL) == 0);
    assert(SharedRingBuffer_GetSampleRate(NULL) == 48000);
    assert(SharedRingBuffer_GetWriteHead(NULL) == 0);
    assert(SharedRingBuffer_GetReadHead(NULL) == 0);
}

/* ---------- shm size-mismatch recovery (regression for v0.6.5) ---------- */

/* macOS POSIX shm fixes a segment's size on first ftruncate; subsequent
 * ftruncate calls return EINVAL. A pre-existing segment at the wrong size
 * (left over from an older build with a different kSHMSize, or created at
 * 0 bytes by an external tool) is therefore unrecoverable except via
 * shm_unlink + recreate. These tests pre-create a wrong-sized segment and
 * assert that the writer-open path recovers it.
 */

/* Returns the current size of the segment by opening read-only and fstat'ing.
 * Returns -1 on error. */
static off_t shm_current_size(void) {
    int fd = shm_open(kSHM_Name, O_RDWR, 0);
    if (fd < 0) return -1;
    struct stat st;
    int rc = fstat(fd, &st);
    close(fd);
    return rc == 0 ? st.st_size : -1;
}

static void test_recovers_from_zero_sized_shm_segment(void) {
    /* Start clean. */
    shm_unlink(kSHM_Name);

    /* Pre-create the segment at size 0 (mirrors what an external probe
     * with shm_open + close would leave behind). */
    int fd = shm_open(kSHM_Name, O_CREAT | O_EXCL | O_RDWR, 0666);
    assert(fd >= 0);
    close(fd);
    assert(shm_current_size() == 0);

    /* Writer-open must recover. macOS rounds shm segments up to a page
     * boundary, so st_size is typically larger than kSHMSize even right
     * after ftruncate; check `>=` rather than equality. */
    SharedRingBuffer* w = SharedRingBuffer_CreateOrOpen(1);
    assert(w != NULL);
    assert(shm_current_size() >= (off_t)SharedRingBuffer_GetSHMSize());

    /* And the recovered segment must be functional end-to-end. */
    SharedRingBuffer* r = SharedRingBuffer_CreateOrOpen(0);
    assert(r != NULL);
    float src[4] = {0.25f, -0.5f, 0.75f, -0.125f};
    SharedRingBuffer_Write(w, src, 2);
    float dst[4] = {0};
    uint64_t read = SharedRingBuffer_Read(r, dst, 2);
    assert(read == 2);
    for (int i = 0; i < 4; i++) assert(fabsf(dst[i] - src[i]) < 1e-6f);

    SharedRingBuffer_Close(r);
    SharedRingBuffer_Close(w);
    SharedRingBuffer_Destroy();
}

static void test_recovers_from_wrong_nonzero_sized_shm_segment(void) {
    /* Simulates a stale segment from an older build whose kSHMSize was
     * different — the segment exists at some non-zero size that doesn't
     * match the current build. ftruncate on this segment is locked
     * (macOS one-shot-size); recovery requires shm_unlink + recreate. */
    shm_unlink(kSHM_Name);

    int fd = shm_open(kSHM_Name, O_CREAT | O_EXCL | O_RDWR, 0666);
    assert(fd >= 0);
    /* Pick a wrong size that's smaller than kSHMSize. macOS rounds shm
     * sizes up to a page boundary, so the actual stored size may exceed
     * our ftruncate request — we only assert that it lands somewhere
     * below kSHMSize, which is what triggers the library's recovery. */
    const off_t wrong_size = 4096;
    assert(wrong_size < (off_t)SharedRingBuffer_GetSHMSize());
    assert(ftruncate(fd, wrong_size) == 0);
    close(fd);
    assert(shm_current_size() < (off_t)SharedRingBuffer_GetSHMSize());

    SharedRingBuffer* w = SharedRingBuffer_CreateOrOpen(1);
    assert(w != NULL);
    assert(shm_current_size() >= (off_t)SharedRingBuffer_GetSHMSize());

    SharedRingBuffer_Close(w);
    SharedRingBuffer_Destroy();
}

/* Writer-open on an already-correctly-sized segment must NOT trigger
 * recovery (no unnecessary shm_unlink + recreate). Macros rounds shm
 * segments up to a page boundary, so a naive `!= kSHMSize` check would
 * force recovery on every open and break the driver-mmap-preservation
 * intent. We verify by checking the inode is stable across opens. */
static void test_no_recovery_when_segment_already_correct_size(void) {
    shm_unlink(kSHM_Name);

    /* First writer-open establishes the segment at the right size. */
    SharedRingBuffer* w1 = SharedRingBuffer_CreateOrOpen(1);
    assert(w1 != NULL);
    int fd1 = shm_open(kSHM_Name, O_RDWR, 0);
    assert(fd1 >= 0);
    struct stat before;
    assert(fstat(fd1, &before) == 0);
    close(fd1);
    SharedRingBuffer_Close(w1);

    /* Second writer-open must reuse the same kernel object — same inode. */
    SharedRingBuffer* w2 = SharedRingBuffer_CreateOrOpen(1);
    assert(w2 != NULL);
    int fd2 = shm_open(kSHM_Name, O_RDWR, 0);
    assert(fd2 >= 0);
    struct stat after;
    assert(fstat(fd2, &after) == 0);
    close(fd2);

    assert(before.st_ino == after.st_ino);

    SharedRingBuffer_Close(w2);
    SharedRingBuffer_Destroy();
}

/* ---------- Writer + Reader Simulation ---------- */

static void test_writer_reader_interleaved(void) {
    SharedRingBuffer* w = fresh_writer();
    SharedRingBuffer* r = SharedRingBuffer_CreateOrOpen(0);
    assert(r != NULL);

    /* Simulate real-time: small writes followed by small reads. */
    uint32_t chunk = 128;
    uint32_t iterations = 200;
    float* src = calloc(chunk * kNumChannels, sizeof(float));
    float* dst = calloc(chunk * kNumChannels, sizeof(float));
    assert(src && dst);

    for (uint32_t iter = 0; iter < iterations; iter++) {
        for (uint32_t i = 0; i < chunk * kNumChannels; i++)
            src[i] = (float)(iter * chunk * kNumChannels + i);

        SharedRingBuffer_Write(w, src, chunk);
        uint64_t read = SharedRingBuffer_Read(r, dst, chunk);
        assert(read == chunk);

        for (uint32_t i = 0; i < chunk * kNumChannels; i++) {
            assert(fabsf(dst[i] - src[i]) < 1e-6f);
        }
    }

    /* Verify heads are consistent. */
    uint64_t expected = (uint64_t)chunk * iterations;
    assert(SharedRingBuffer_GetWriteHead(w) == expected);
    assert(SharedRingBuffer_GetReadHead(r) == expected);

    free(src);
    free(dst);
    SharedRingBuffer_Close(r);
    SharedRingBuffer_Close(w);
    SharedRingBuffer_Destroy();
}

/* ---------- Main ---------- */

int main(void) {
    printf("SharedRingBuffer unit tests\n");
    printf("===========================\n");

    printf("\nWrite/Read Roundtrip:\n");
    RUN_TEST(test_write_read_simple);
    RUN_TEST(test_write_read_large_block);
    RUN_TEST(test_multiple_writes_single_read);
    RUN_TEST(test_write_read_preserves_channel_interleaving);

    printf("\nWraparound:\n");
    RUN_TEST(test_write_wraps_around_buffer);
    RUN_TEST(test_write_exactly_fills_buffer);
    RUN_TEST(test_multiple_wraparounds);

    printf("\nHead Tracking:\n");
    RUN_TEST(test_write_head_advances);
    RUN_TEST(test_read_head_advances);
    RUN_TEST(test_heads_monotonically_increase);

    printf("\nActive / SampleRate:\n");
    RUN_TEST(test_default_sample_rate);
    RUN_TEST(test_set_sample_rate);
    RUN_TEST(test_default_active_is_zero);
    RUN_TEST(test_set_active);

    printf("\nEdge Cases:\n");
    RUN_TEST(test_zero_length_write);
    RUN_TEST(test_read_more_than_available);
    RUN_TEST(test_read_empty_buffer);

    printf("\nNULL Safety:\n");
    RUN_TEST(test_null_write);
    RUN_TEST(test_null_read);

    printf("\nshm size-mismatch recovery (regression v0.6.5):\n");
    RUN_TEST(test_recovers_from_zero_sized_shm_segment);
    RUN_TEST(test_recovers_from_wrong_nonzero_sized_shm_segment);
    RUN_TEST(test_no_recovery_when_segment_already_correct_size);

    printf("\nWriter + Reader Simulation:\n");
    RUN_TEST(test_writer_reader_interleaved);

    printf("\n===========================\n");
    printf("%d/%d tests passed.\n", tests_passed, tests_run);

    /* Clean up shm just in case. */
    SharedRingBuffer_Destroy();

    return (tests_passed == tests_run) ? 0 : 1;
}
