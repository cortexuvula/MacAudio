import Foundation

final class SharedRingBufferWriter {
    private var ringBuffer: OpaquePointer?

    var isOpen: Bool { ringBuffer != nil }

    func open() throws {
        guard let rb = SharedRingBuffer_CreateOrOpen(1) else {
            throw NSError(domain: "MacAudio", code: 1,
                          userInfo: [NSLocalizedDescriptionKey: "Failed to create/open shared ring buffer"])
        }
        ringBuffer = rb
        SharedRingBuffer_SetActive(rb, 1)
        SharedRingBuffer_SetSampleRate(rb, UInt32(AudioConstants.defaultSampleRate))
    }

    func write(frames: UnsafePointer<Float>, frameCount: UInt32) {
        guard let rb = ringBuffer else { return }
        SharedRingBuffer_Write(rb, frames, frameCount)
    }

    func close() {
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
        close()
    }
}
