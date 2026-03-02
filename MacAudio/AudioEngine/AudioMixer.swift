import CoreAudio
import os

final class AudioMixer {
    private let micCapture = MicCapture()
    private let systemCapture = SystemAudioCapture()
    private let ringBufferWriter = SharedRingBufferWriter()
    private let logger = Logger(subsystem: "com.macaudio.app", category: "mixer")

    // Lock-free mic buffer for cross-thread communication
    private let micBufferLock = NSLock()
    private var micBuffer = [Float](repeating: 0, count: Int(AudioConstants.ringBufferFrames) * Int(AudioConstants.numChannels))
    private var micWritePos: Int = 0
    private var micReadPos: Int = 0

    private(set) var isRunning = false
    private(set) var systemCaptureError: String?

    var micGain: Float = 0.7
    var systemGain: Float = 0.7

    func start(micDeviceID: AudioDeviceID? = nil) throws {
        guard !isRunning else { return }

        try ringBufferWriter.open()
        logger.info("Shared ring buffer opened")

        // Set up mic capture
        if let deviceID = micDeviceID {
            micCapture.setInputDevice(deviceID)
        }
        micCapture.onAudioReceived = { [weak self] floatPtr, frameCount, channelCount in
            self?.handleMicAudio(floatPtr, frameCount: frameCount, channelCount: Int(channelCount))
        }

        // Set up system audio capture
        systemCapture.onAudioReceived = { [weak self] bufferList, frameCount in
            self?.handleSystemAudio(bufferList, frameCount: frameCount)
        }

        // Start captures
        try micCapture.start()
        logger.info("Mic capture started")

        do {
            try systemCapture.start()
            logger.info("System audio capture started")
            systemCaptureError = nil
        } catch {
            logger.error("System audio capture failed: \(error.localizedDescription)")
            systemCaptureError = error.localizedDescription
            // Continue with mic only
        }

        isRunning = true
    }

    func stop() {
        guard isRunning else { return }
        isRunning = false

        micCapture.stop()
        systemCapture.stop()
        ringBufferWriter.close()
        logger.info("Audio mixer stopped")
    }

    func setMicDevice(_ deviceID: AudioDeviceID) {
        micCapture.setInputDevice(deviceID)
    }

    // MARK: - Audio Callbacks

    private func handleMicAudio(_ floatPtr: UnsafePointer<Float>, frameCount: UInt32, channelCount: Int) {
        let frames = Int(frameCount)
        let outChannels = Int(AudioConstants.numChannels)
        let bufferCapacity = micBuffer.count

        micBufferLock.lock()
        defer { micBufferLock.unlock() }

        for frame in 0..<frames {
            let writeIdx = (micWritePos + frame * outChannels) % bufferCapacity

            if channelCount >= outChannels {
                // Interleaved stereo or more — copy directly
                for ch in 0..<outChannels {
                    micBuffer[(writeIdx + ch) % bufferCapacity] = floatPtr[frame * channelCount + ch]
                }
            } else {
                // Mono mic — duplicate to stereo
                let sample = floatPtr[frame]
                for ch in 0..<outChannels {
                    micBuffer[(writeIdx + ch) % bufferCapacity] = sample
                }
            }
        }
        micWritePos = (micWritePos + frames * outChannels) % bufferCapacity
    }

    private func handleSystemAudio(_ bufferList: UnsafePointer<AudioBufferList>, frameCount: UInt32) {
        let channels = Int(AudioConstants.numChannels)
        let frames = Int(frameCount)
        guard frames > 0 else { return }

        // Allocate mixing buffer
        var mixedBuffer = [Float](repeating: 0, count: frames * channels)

        // Copy system audio into mix buffer with gain
        let numBuffers = Int(bufferList.pointee.mNumberBuffers)
        guard numBuffers >= 1 else { return }

        // Access the first AudioBuffer directly
        let firstBuffer = bufferList.pointee.mBuffers
        if let data = firstBuffer.mData {
            let sysFloats = data.assumingMemoryBound(to: Float.self)
            let sysChannels = Int(firstBuffer.mNumberChannels)
            let sysGain = self.systemGain

            if sysChannels >= channels {
                // Interleaved stereo or more
                for i in 0..<(frames * channels) {
                    mixedBuffer[i] = sysFloats[i] * sysGain
                }
            } else if sysChannels == 1 {
                // Mono system audio -> duplicate to stereo
                for frame in 0..<frames {
                    let sample = sysFloats[frame] * sysGain
                    mixedBuffer[frame * channels] = sample
                    mixedBuffer[frame * channels + 1] = sample
                }
            }
        }

        // Read mic audio and mix in
        micBufferLock.lock()
        let micGainVal = self.micGain
        let bufferCapacity = micBuffer.count
        let available: Int
        if micWritePos >= micReadPos {
            available = micWritePos - micReadPos
        } else {
            available = bufferCapacity - micReadPos + micWritePos
        }
        let samplesToRead = min(available, frames * channels)

        for i in 0..<samplesToRead {
            let readIdx = (micReadPos + i) % bufferCapacity
            mixedBuffer[i] += micBuffer[readIdx] * micGainVal
        }
        micReadPos = (micReadPos + samplesToRead) % bufferCapacity
        micBufferLock.unlock()

        // Hard clip
        for i in 0..<mixedBuffer.count {
            mixedBuffer[i] = max(-1.0, min(1.0, mixedBuffer[i]))
        }

        // Write to shared ring buffer
        mixedBuffer.withUnsafeBufferPointer { bufPtr in
            if let base = bufPtr.baseAddress {
                ringBufferWriter.write(frames: base, frameCount: frameCount)
            }
        }
    }

    deinit {
        stop()
    }
}
