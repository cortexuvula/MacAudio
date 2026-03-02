#include "SharedRingBuffer.h"
#include <stdatomic.h>
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

static const uint64_t kSHMSize = sizeof(struct SharedRingBuffer);

uint64_t SharedRingBuffer_GetSHMSize(void) {
    return kSHMSize;
}

SharedRingBuffer* SharedRingBuffer_CreateOrOpen(int forWriting) {
    if (forWriting) {
        // Remove stale shm from previous sessions, then create fresh.
        // The driver re-opens in StartIO so it will pick up the new segment.
        shm_unlink(kSHM_Name);
    }

    int flags = forWriting ? (O_CREAT | O_RDWR) : O_RDWR;
    int fd = shm_open(kSHM_Name, flags, 0666);
    if (fd < 0) {
        perror("shm_open");
        return NULL;
    }

    if (forWriting) {
        if (ftruncate(fd, (off_t)kSHMSize) != 0) {
            perror("ftruncate");
            close(fd);
            return NULL;
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
        memset(rb->buffer, 0, sizeof(rb->buffer));
        atomic_store_explicit(&rb->writeHeadFrames, 0, memory_order_release);
        atomic_store_explicit(&rb->readHeadFrames, 0, memory_order_release);
        atomic_store_explicit(&rb->sampleRate, 48000, memory_order_release);
        atomic_store_explicit(&rb->active, 0, memory_order_release);
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
