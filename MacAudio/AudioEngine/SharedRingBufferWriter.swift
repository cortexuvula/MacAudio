import Foundation
import os

final class SharedRingBufferWriter {
    // Touched from both the control thread (open/close) and the realtime audio
    // IO thread (write, from AudioMixer). An actor is the wrong tool — the IO
    // path cannot suspend — so guard the pointer with an os_unfair_lock, whose
    // critical sections are a pointer load/store plus the in-flight memcpy.
    private var ringBuffer: OpaquePointer?
    private var lock = os_unfair_lock_s()

    var isOpen: Bool {
        os_unfair_lock_lock(&lock)
        defer { os_unfair_lock_unlock(&lock) }
        return ringBuffer != nil
    }

    func open() throws {
        os_unfair_lock_lock(&lock)
        defer { os_unfair_lock_unlock(&lock) }
        try openLocked()
    }

    /// Must be called holding `lock`.
    private func openLocked() throws {
        // Close any existing mapping first. Without this, a second open()
        // would overwrite `ringBuffer` and leak the prior mmap (its memory
        // would never be munmap'd until process exit).
        closeLocked()

        guard let rb = SharedRingBuffer_CreateOrOpen(1) else {
            throw NSError(domain: "MacAudio", code: 1,
                          userInfo: [NSLocalizedDescriptionKey: "Failed to create/open shared ring buffer"])
        }
        ringBuffer = rb
        SharedRingBuffer_SetActive(rb, 1)
        SharedRingBuffer_SetSampleRate(rb, UInt32(AudioConstants.defaultSampleRate))
    }

    func write(frames: UnsafePointer<Float>, frameCount: UInt32) {
        os_unfair_lock_lock(&lock)
        // Resolve the pointer under the lock, but keep the lock held across the
        // write so the mapping can't be munmap'd mid-memcpy by a concurrent
        // close() on another thread.
        guard let rb = ringBuffer else {
            os_unfair_lock_unlock(&lock)
            return
        }
        SharedRingBuffer_Write(rb, frames, frameCount)
        os_unfair_lock_unlock(&lock)
    }

    func close() {
        os_unfair_lock_lock(&lock)
        defer { os_unfair_lock_unlock(&lock) }
        closeLocked()
    }

    /// Must be called holding `lock`.
    private func closeLocked() {
        guard let rb = ringBuffer else { return }
        SharedRingBuffer_SetActive(rb, 0)
        SharedRingBuffer_Close(rb)
        ringBuffer = nil
    }

    func destroy() {
        close()
        SharedRingBuffer_Destroy()
    }

    deinit {
        // No isolation concern in deinit: the lock is a value owned by this
        // instance, and by deinit no other reference can exist.
        closeLocked()
    }
}
