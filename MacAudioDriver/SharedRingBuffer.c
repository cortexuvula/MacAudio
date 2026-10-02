#include "SharedRingBuffer.h"
#include <stdatomic.h>
#include <stddef.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
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
    // Pid of the process that currently owns the writer side; 0 = unowned.
    // Carved out of what used to be header padding, so kSHMSize and the
    // buffer offset are unchanged.
    _Atomic int32_t writerOwnerPid;
    // Generation counter, bumped on every writer-ownership acquisition. A
    // reader still mapped to the segment keeps seeing these pages even after
    // the segment is unlinked, so the bump (including the one from teardown's
    // final acquire, just before unlink) is how a reader detects that the
    // writer side went away and the segment was replaced. Also carved out of
    // former padding; kSHMSize and the buffer offset are unchanged.
    _Atomic uint32_t epoch;
    uint8_t _padding[64 - (2 * 8 + 2 * 4 + 4 + 4)];
    float buffer[kMaxFrameSize];
};

_Static_assert(offsetof(struct SharedRingBuffer, epoch) == 28, "epoch must sit in former padding right after writerOwnerPid");
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
    // Mode 0666 is intentional: the writer (user app) and the reader
    // (coreaudiod, a different system UID) must both map this segment, so a
    // restrictive 0600 would break the driver's read path. Safety against a
    // corruptable/malicious segment comes from defensive head capping in
    // SharedRingBuffer_Read, not from access control. Do not "tighten" this.
    int fd = shm_open(kSHM_Name, flags, 0666);
    if (fd < 0) {
        perror("shm_open");
        return NULL;
    }

    if (forWriting) {
        struct stat st;
        // Use `<` not `!=`: macOS rounds POSIX shm segments up to a page
        // boundary, so st.st_size is typically larger than kSHMSize even
        // on a freshly-ftruncate'd segment. Comparing for inequality
        // would force a unnecessary shm_unlink + recreate on every
        // writer-open. We only need to recover when the segment is too
        // small to mmap kSHMSize bytes from.
        if (fstat(fd, &st) != 0 || st.st_size < (off_t)kSHMSize) {
            // macOS POSIX shm fixes a segment's size on first ftruncate;
            // any later ftruncate (even from a fresh fd, even after the
            // creator exits) returns EINVAL. So a too-small segment —
            // left over from an older build with a smaller kSHMSize, or
            // created at 0 bytes by an external tool — is unrecoverable
            // except via shm_unlink + recreate. This overrides the
            // "don't unlink" guidance above for the size-mismatch case:
            // a driver still mmap'd to a too-small segment is already
            // broken.
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
        // Establish exclusive ownership BEFORE resetting any shared state.
        // Writer-open resets heads/active/contents below; without this gate a
        // second live app instance would silence and corrupt the first
        // writer's stream. Refused opens munmap and return NULL without
        // touching the segment. Same-process re-open and takeover from a
        // crashed owner are allowed inside TryAcquire.
        if (!SharedRingBuffer_TryAcquireWriterOwnership(rb)) {
            munmap(rb, (size_t)kSHMSize);
            return NULL;
        }
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

// Exclusive writer ownership
//
// The writer-open path resets the shared heads/active/contents, so exactly one
// live writer may use the segment at a time. Ownership is the acquiring pid
// stamped into the segment header; writer-open acquires it before any reset,
// the owner releases it on close, and teardown (unlink) is owner-only. An
// owner that crashed without releasing is detected via a dead pid and taken
// over on the next writer-open. Failing closed is deliberate: if pid reuse
// ever makes a dead owner look live, new writers are refused until the
// segment is unlinked rather than corrupting a possibly-live stream.

static int shm_owner_pid_is_dead(int32_t ownerPid) {
    // kill(pid, 0) probes existence: -1/ESRCH means the process is gone.
    // EPERM would mean "exists but not ours" — still a live owner.
    errno = 0;
    return kill((pid_t)ownerPid, 0) != 0 && errno == ESRCH;
}

// Bump the generation counter on every successful writer-ownership
// acquisition (fresh acquire, same-process re-acquire, dead-owner takeover).
// Readers treat an epoch change as "the writer side was replaced". The bump
// lands on the segment's own pages, so a still-mapped reader — the driver,
// which cannot reopen from its realtime path — observes it and knows to
// reattach. This includes the acquire inside DestroyIfUnowned: it bumps the
// old inode right before unlinking, so a relaunched app's fresh segment is
// what a reattaching reader finds, not a frozen orphan mapping.
static void bump_epoch(SharedRingBuffer* rb) {
    atomic_fetch_add_explicit(&rb->epoch, 1, memory_order_acq_rel);
}

int SharedRingBuffer_TryAcquireWriterOwnership(SharedRingBuffer* rb) {
    if (!rb) return 0;
    int32_t self = (int32_t)getpid();
    int32_t expected = 0;
    if (atomic_compare_exchange_strong_explicit(&rb->writerOwnerPid, &expected, self,
                                                memory_order_acq_rel, memory_order_acquire)) {
        bump_epoch(rb);
        return 1;
    }
    if (expected == self) {
        // Re-acquire by the owning process (e.g. Stop→Start in one app).
        bump_epoch(rb);
        return 1;
    }
    if (expected > 0 && shm_owner_pid_is_dead(expected)) {
        // The previous owner crashed without releasing. CAS on the dead pid
        // so exactly one of several competing writers wins the takeover.
        int32_t dead = expected;
        if (atomic_compare_exchange_strong_explicit(&rb->writerOwnerPid, &dead, self,
                                                    memory_order_acq_rel, memory_order_acquire)) {
            bump_epoch(rb);
            return 1;
        }
    }
    return 0;
}

void SharedRingBuffer_ReleaseWriterOwnership(SharedRingBuffer* rb) {
    if (!rb) return;
    int32_t self = (int32_t)getpid();
    int32_t expected = self;
    // Release only if we are the owner; a no-op otherwise.
    atomic_compare_exchange_strong_explicit(&rb->writerOwnerPid, &expected, 0,
                                            memory_order_release, memory_order_relaxed);
}

int SharedRingBuffer_HasLiveWriterOwner(void) {
    int fd = shm_open(kSHM_Name, O_RDWR, 0666);
    if (fd < 0) {
        // No segment → nobody owns it.
        return 0;
    }
    SharedRingBuffer* rb = (SharedRingBuffer*)mmap(
        NULL, (size_t)kSHMSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (rb == MAP_FAILED) {
        return 0;
    }
    int32_t owner = atomic_load_explicit(&rb->writerOwnerPid, memory_order_acquire);
    int live = (owner != 0 && owner != (int32_t)getpid() && !shm_owner_pid_is_dead(owner));
    munmap(rb, (size_t)kSHMSize);
    return live;
}

int SharedRingBuffer_DestroyIfUnowned(void) {
    // Teardown is an owner-only operation: unlink only when no LIVE writer
    // owns the segment. Acquiring ownership here (which also takes over from
    // a crashed owner) makes the check-and-unlink atomic against new writers.
    int fd = shm_open(kSHM_Name, O_RDWR, 0666);
    if (fd < 0) {
        return (errno == ENOENT);  // already gone counts as done
    }
    SharedRingBuffer* rb = (SharedRingBuffer*)mmap(
        NULL, (size_t)kSHMSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (rb == MAP_FAILED) {
        return 0;
    }
    int owned = SharedRingBuffer_TryAcquireWriterOwnership(rb);
    if (owned) {
        // Silence any still-connected reader promptly; the mapping dies with
        // the unlinked inode once the last reader closes it.
        atomic_store_explicit(&rb->active, 0, memory_order_release);
        shm_unlink(kSHM_Name);
    }
    munmap(rb, (size_t)kSHMSize);
    return owned;
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

    // SPSC contract: the writer owns ONLY writeHeadFrames. It must never touch
    // readHeadFrames — that belongs to the reader. On overrun (reader fell
    // behind) the writer simply overwrites the oldest unread samples in place
    // and advances writeHead; the reader detects that it was lapped and resyncs
    // its own readHead forward in SharedRingBuffer_Read. Mutating readHead here
    // would race the reader's own update and could lose/corrupt head state.
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

uint32_t SharedRingBuffer_GetEpoch(SharedRingBuffer* rb) {
    if (!rb) return 0;
    return atomic_load_explicit(&rb->epoch, memory_order_acquire);
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

    // The reader owns readHeadFrames exclusively. If the writer has lapped us
    // (or heads are otherwise corrupted/stale), 'available' can exceed the
    // buffer capacity. Resync our own readHead forward to one buffer behind the
    // writer and clamp available — this also bounds every memcpy below to
    // kRingBufferFrames regardless of what the heads contain, closing the OOB
    // read path corrupted heads would otherwise enable.
    if (available > kRingBufferFrames) {
        readHead = writeHead - kRingBufferFrames;
        available = kRingBufferFrames;
    }

    uint32_t framesToRead = (available >= frameCount) ? frameCount : (uint32_t)available;
    if (framesToRead > kRingBufferFrames) framesToRead = kRingBufferFrames;

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
