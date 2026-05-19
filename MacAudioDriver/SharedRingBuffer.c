#include "SharedRingBuffer.h"
#include <stdatomic.h>
#include <stddef.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

struct SharedRingBuffer {
    _Atomic uint64_t writeHeadFrames;
    _Atomic uint64_t readHeadFrames;
    _Atomic uint32_t sampleRate;
    _Atomic uint32_t active;
    uint8_t _padding[64 - (2 * 8 + 2 * 4)];
    float buffer[kMaxFrameSize];
};

_Static_assert(offsetof(struct SharedRingBuffer, buffer) == 64, "buffer must be at cache-line offset 64");

static const uint64_t kSHMSize = sizeof(struct SharedRingBuffer);

uint64_t SharedRingBuffer_GetSHMSize(void) {
    return kSHMSize;
}

SharedRingBuffer* SharedRingBuffer_CreateOrOpen(int forWriting) {
    // Important: do NOT shm_unlink on writer open. If the driver already has
    // this segment mmap'd from a previous Start cycle and a recording client
    // is still streaming, unlink+recreate would leave the driver's mmap
    // pointing at an orphan inode while the writer fills a fresh one — the
    // driver would output silence indefinitely. Reusing the existing segment
    // keeps reader and writer pointing at the same memory across Stop→Start.

    int flags = forWriting ? (O_CREAT | O_RDWR) : O_RDWR;
    int fd = shm_open(kSHM_Name, flags, 0666);
    if (fd < 0) {
        perror("shm_open");
        return NULL;
    }

    if (forWriting) {
        struct stat st;
        if (fstat(fd, &st) != 0 || st.st_size != (off_t)kSHMSize) {
            // macOS POSIX shm fixes a segment's size on first ftruncate;
            // any later ftruncate (even from a fresh fd, even after the
            // creator exits) returns EINVAL. So a stale segment at the
            // wrong size — left over from an older build with a different
            // kSHMSize, or created at 0 bytes by an external tool — is
            // unrecoverable except via shm_unlink + recreate. This
            // overrides the "don't unlink" guidance above for the
            // size-mismatch case: a driver still mmap'd to a wrong-sized
            // segment is already broken.
            close(fd);
            shm_unlink(kSHM_Name);
            fd = shm_open(kSHM_Name, O_CREAT | O_RDWR, 0666);
            if (fd < 0) {
                perror("shm_open");
                return NULL;
            }
            if (ftruncate(fd, (off_t)kSHMSize) != 0) {
                perror("ftruncate");
                close(fd);
                return NULL;
            }
        }
    }

    int prot = PROT_READ | PROT_WRITE;
    SharedRingBuffer* rb = (SharedRingBuffer*)mmap(
        NULL, (size_t)kSHMSize, prot, MAP_SHARED, fd, 0);
    close(fd);

    if (rb == MAP_FAILED) {
        perror("mmap");
        return NULL;
    }

    if (forWriting) {
        // Force inactive while we reset state so any in-flight reader bails
        // to silence (driver checks `active` every IO cycle). Caller will
        // SetActive(1) once it's ready to push audio.
        atomic_store_explicit(&rb->active, 0, memory_order_release);
        // Reset heads. Without this, leftover offsets from a prior session
        // can make available = writeHead - readHead look enormous and the
        // reader will memcpy garbage from the buffer.
        atomic_store_explicit(&rb->writeHeadFrames, 0, memory_order_release);
        atomic_store_explicit(&rb->readHeadFrames, 0, memory_order_release);
        atomic_store_explicit(&rb->sampleRate, 48000, memory_order_release);
        memset(rb->buffer, 0, sizeof(rb->buffer));
    }

    return rb;
}

void SharedRingBuffer_Close(SharedRingBuffer* rb) {
    if (rb) {
        munmap(rb, (size_t)kSHMSize);
    }
}

void SharedRingBuffer_Destroy(void) {
    shm_unlink(kSHM_Name);
}

// Writer accessors

void SharedRingBuffer_SetActive(SharedRingBuffer* rb, uint32_t active) {
    if (rb) atomic_store_explicit(&rb->active, active, memory_order_release);
}

void SharedRingBuffer_SetSampleRate(SharedRingBuffer* rb, uint32_t rate) {
    if (rb) atomic_store_explicit(&rb->sampleRate, rate, memory_order_release);
}

uint64_t SharedRingBuffer_GetWriteHead(SharedRingBuffer* rb) {
    if (!rb) return 0;
    return atomic_load_explicit(&rb->writeHeadFrames, memory_order_acquire);
}

void SharedRingBuffer_Write(SharedRingBuffer* rb, const float* frames, uint32_t frameCount) {
    if (!rb || !frames || frameCount == 0) return;

    uint64_t writeHead = atomic_load_explicit(&rb->writeHeadFrames, memory_order_acquire);
    uint64_t readHead  = atomic_load_explicit(&rb->readHeadFrames,  memory_order_acquire);

    // If the reader has fallen far enough behind that this write would overrun
    // unread data, drop the oldest by advancing readHead. This keeps the buffer
    // bounded and guarantees the reader will not see torn data mid-memcpy.
    // (Reader stalls happen on driver glitches; preferring to drop oldest keeps
    // the live audio path latency-bounded.)
    uint64_t inUse = writeHead - readHead;
    if (inUse + frameCount > kRingBufferFrames) {
        uint64_t needToAdvance = inUse + frameCount - kRingBufferFrames;
        atomic_store_explicit(&rb->readHeadFrames,
                              readHead + needToAdvance, memory_order_release);
    }

    uint32_t offset = (uint32_t)(writeHead & (kRingBufferFrames - 1));
    uint32_t remaining = frameCount;
    uint32_t srcOffset = 0;

    while (remaining > 0) {
        uint32_t spaceToEnd = kRingBufferFrames - offset;
        uint32_t chunk = remaining < spaceToEnd ? remaining : spaceToEnd;
        uint32_t sampleCount = chunk * kNumChannels;

        memcpy(&rb->buffer[offset * kNumChannels],
               &frames[srcOffset * kNumChannels],
               sampleCount * sizeof(float));

        remaining -= chunk;
        srcOffset += chunk;
        offset = 0;
    }

    atomic_store_explicit(&rb->writeHeadFrames,
                          writeHead + frameCount, memory_order_release);
}

// Reader accessors

uint32_t SharedRingBuffer_GetActive(SharedRingBuffer* rb) {
    if (!rb) return 0;
    return atomic_load_explicit(&rb->active, memory_order_acquire);
}

uint32_t SharedRingBuffer_GetSampleRate(SharedRingBuffer* rb) {
    if (!rb) return 48000;
    return atomic_load_explicit(&rb->sampleRate, memory_order_acquire);
}

uint64_t SharedRingBuffer_GetReadHead(SharedRingBuffer* rb) {
    if (!rb) return 0;
    return atomic_load_explicit(&rb->readHeadFrames, memory_order_acquire);
}

uint64_t SharedRingBuffer_Read(SharedRingBuffer* rb, float* outBuffer, uint32_t frameCount) {
    if (!rb || !outBuffer || frameCount == 0) return 0;

    uint64_t readHead = atomic_load_explicit(&rb->readHeadFrames, memory_order_acquire);
    uint64_t writeHead = atomic_load_explicit(&rb->writeHeadFrames, memory_order_acquire);
    uint64_t available = writeHead - readHead;

    uint32_t framesToRead = (available >= frameCount) ? frameCount : (uint32_t)available;

    if (framesToRead > 0) {
        uint32_t offset = (uint32_t)(readHead & (kRingBufferFrames - 1));
        uint32_t firstChunk = framesToRead;
        if (offset + framesToRead > kRingBufferFrames) {
            firstChunk = kRingBufferFrames - offset;
        }
        uint32_t secondChunk = framesToRead - firstChunk;

        memcpy(outBuffer,
               &rb->buffer[offset * kNumChannels],
               firstChunk * kNumChannels * sizeof(float));

        if (secondChunk > 0) {
            memcpy(&outBuffer[firstChunk * kNumChannels],
                   rb->buffer,
                   secondChunk * kNumChannels * sizeof(float));
        }

        atomic_store_explicit(&rb->readHeadFrames,
                              readHead + framesToRead, memory_order_release);
    }

    return framesToRead;
}
