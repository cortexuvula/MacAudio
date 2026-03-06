import AVFAudio
import CoreAudio
import os

final class AudioMixer {
    private let micCapture = MicCapture()
    private let systemCapture = SystemAudioCapture()
    private let ringBufferWriter = SharedRingBufferWriter()
    private let logger = Logger(subsystem: "com.macaudio.app", category: "mixer")

    // Lock-free mic buffer for cross-thread communication
    private var micBufferLock = os_unfair_lock()
    private var micBuffer = [Float](repeating: 0, count: Int(AudioConstants.ringBufferFrames) * Int(AudioConstants.numChannels))
    private var micWritePos: Int = 0
    private var micReadPos: Int = 0

    private var micConverter: SampleRateConverter?
    private var systemConverter: SampleRateConverter?

    private(set) var isRunning = false
    private(set) var systemCaptureActive = false
    private(set) var systemCaptureError: String?

    // Atomic gain accessors via os_unfair_lock
    private var _micGain: Float = AudioConstants.defaultGain
    private var _systemGain: Float = AudioConstants.defaultGain
    private var gainLock = os_unfair_lock()

    var micGain: Float {
        get { os_unfair_lock_lock(&gainLock); defer { os_unfair_lock_unlock(&gainLock) }; return _micGain }
        set { os_unfair_lock_lock(&gainLock); _micGain = newValue; os_unfair_lock_unlock(&gainLock) }
    }
    var systemGain: Float {
        get { os_unfair_lock_lock(&gainLock); defer { os_unfair_lock_unlock(&gainLock) }; return _systemGain }
        set { os_unfair_lock_lock(&gainLock); _systemGain = newValue; os_unfair_lock_unlock(&gainLock) }
    }

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

        // Create mic sample rate converter if needed
        let destRate = AudioConstants.defaultSampleRate
        if micCapture.sampleRate > 0 && micCapture.sampleRate != destRate {
            micConverter = SampleRateConverter(sourceRate: micCapture.sampleRate, destRate: destRate)
            logger.info("Mic SRC: \(self.micCapture.sampleRate)Hz -> \(destRate)Hz")
        } else {
            micConverter = nil
        }

        do {
            try systemCapture.start()
            systemCaptureActive = true
            logger.info("System audio capture started")
            systemCaptureError = nil

            // Create system audio sample rate converter if needed
            if systemCapture.sampleRate > 0 && systemCapture.sampleRate != destRate {
                systemConverter = SampleRateConverter(sourceRate: systemCapture.sampleRate, destRate: destRate)
                logger.info("System SRC: \(self.systemCapture.sampleRate)Hz -> \(destRate)Hz")
            } else {
                systemConverter = nil
            }
        } catch {
            logger.error("System audio capture failed: \(error.localizedDescription)")
            systemCaptureActive = false
            systemCaptureError = error.localizedDescription
            systemConverter = nil
            // Continue with mic only
        }

        isRunning = true
    }

    func stop() {
        guard isRunning else { return }
        isRunning = false
        systemCaptureActive = false

        micCapture.stop()
        systemCapture.stop()
        micConverter = nil
        systemConverter = nil
        ringBufferWriter.close()
        logger.info("Audio mixer stopped")
    }

    func destroySharedMemory() {
        ringBufferWriter.destroy()
    }

    func setMicDevice(_ deviceID: AudioDeviceID) {
        micCapture.setInputDevice(deviceID)
    }

    // MARK: - Audio Callbacks

    private func handleMicAudio(_ floatPtr: UnsafePointer<Float>, frameCount: UInt32, channelCount: Int) {
        var frames = Int(frameCount)
        let outChannels = Int(AudioConstants.numChannels)

        // Step 1: Upmix mono to stereo if needed, producing interleaved stereo buffer
        var stereoBuffer: [Float]?
        var stereoPtr: UnsafePointer<Float> = floatPtr

        if channelCount < outChannels {
            // Mono -> stereo upmix
            var buf = [Float](repeating: 0, count: frames * outChannels)
            for frame in 0..<frames {
                let sample = floatPtr[frame]
                buf[frame * outChannels] = sample
                buf[frame * outChannels + 1] = sample
            }
            stereoBuffer = buf
        } else if channelCount > outChannels {
            // Downmix to stereo (take first 2 channels)
            var buf = [Float](repeating: 0, count: frames * outChannels)
            for frame in 0..<frames {
                for ch in 0..<outChannels {
                    buf[frame * outChannels + ch] = floatPtr[frame * channelCount + ch]
                }
            }
            stereoBuffer = buf
        }
        // If channelCount == outChannels, use floatPtr directly

        // Step 2: Resample if converter exists
        var finalFrameCount = UInt32(frames)
        if let converter = micConverter {
            let srcPtr: UnsafePointer<Float>
            if let buf = stereoBuffer {
                srcPtr = buf.withUnsafeBufferPointer { $0.baseAddress! }
            } else {
                srcPtr = floatPtr
            }
            if let (resampledPtr, resampledFrames) = converter.convert(srcPtr, frameCount: UInt32(frames)) {
                stereoPtr = resampledPtr
                finalFrameCount = resampledFrames
                frames = Int(resampledFrames)
                stereoBuffer = nil // use resampledPtr instead
            }
        } else if let buf = stereoBuffer {
            // No converter, but we have a stereo buffer from upmix
            // We need to keep stereoBuffer alive and point stereoPtr to it
            // Handle below in the closures
        }

        // Helper to get the final pointer
        let writeFrames = { [stereoBuffer] (body: (UnsafePointer<Float>, Int) -> Void) in
            if let buf = stereoBuffer {
                buf.withUnsafeBufferPointer { bufPtr in
                    body(bufPtr.baseAddress!, frames)
                }
            } else {
                body(stereoPtr, frames)
            }
        }

        if systemCaptureActive {
            // Buffer mic audio for mixing in the system audio callback
            let bufferCapacity = micBuffer.count

            os_unfair_lock_lock(&micBufferLock)
            defer { os_unfair_lock_unlock(&micBufferLock) }

            writeFrames { ptr, frameCount in
                for frame in 0..<frameCount {
                    let writeIdx = (self.micWritePos + frame * outChannels) % bufferCapacity
                    for ch in 0..<outChannels {
                        self.micBuffer[(writeIdx + ch) % bufferCapacity] = ptr[frame * outChannels + ch]
                    }
                }
                self.micWritePos = (self.micWritePos + frameCount * outChannels) % bufferCapacity
            }
        } else {
            // Mic-only mode: apply gain, clip, and write directly to ring buffer
            let gain = self.micGain

            writeFrames { ptr, frameCount in
                var outputBuffer = [Float](repeating: 0, count: frameCount * outChannels)
                for i in 0..<(frameCount * outChannels) {
                    outputBuffer[i] = max(-1.0, min(1.0, ptr[i] * gain))
                }
                outputBuffer.withUnsafeBufferPointer { bufPtr in
                    if let base = bufPtr.baseAddress {
                        self.ringBufferWriter.write(frames: base, frameCount: UInt32(frameCount))
                    }
                }
            }
        }
    }

    private func handleSystemAudio(_ bufferList: UnsafePointer<AudioBufferList>, frameCount: UInt32) {
        let channels = Int(AudioConstants.numChannels)
        var frames = Int(frameCount)
        guard frames > 0 else { return }

        // Extract system audio as interleaved stereo
        let numBuffers = Int(bufferList.pointee.mNumberBuffers)
        guard numBuffers >= 1 else { return }

        let firstBuffer = bufferList.pointee.mBuffers
        guard let data = firstBuffer.mData else { return }

        let sysFloats = data.assumingMemoryBound(to: Float.self)
        let sysChannels = Int(firstBuffer.mNumberChannels)

        // Prepare stereo interleaved pointer for potential resampling
        var stereoBuffer: [Float]?
        var sysPtr: UnsafePointer<Float> = UnsafePointer(sysFloats)

        if sysChannels < channels {
            // Mono -> stereo upmix
            var buf = [Float](repeating: 0, count: frames * channels)
            for frame in 0..<frames {
                let sample = sysFloats[frame]
                buf[frame * channels] = sample
                buf[frame * channels + 1] = sample
            }
            stereoBuffer = buf
        } else if sysChannels > channels {
            // Downmix: take first 2 channels
            var buf = [Float](repeating: 0, count: frames * channels)
            for frame in 0..<frames {
                for ch in 0..<channels {
                    buf[frame * channels + ch] = sysFloats[frame * sysChannels + ch]
                }
            }
            stereoBuffer = buf
        }
        // If sysChannels == channels, use sysFloats directly

        // Resample if converter exists
        var finalFrameCount = UInt32(frames)
        if let converter = systemConverter {
            let srcPtr: UnsafePointer<Float>
            if let buf = stereoBuffer {
                srcPtr = buf.withUnsafeBufferPointer { $0.baseAddress! }
            } else {
                srcPtr = UnsafePointer(sysFloats)
            }
            if let (resampledPtr, resampledFrames) = converter.convert(srcPtr, frameCount: UInt32(frames)) {
                sysPtr = resampledPtr
                finalFrameCount = resampledFrames
                frames = Int(resampledFrames)
                stereoBuffer = nil
            }
        }

        // Build mixing buffer with system audio + gain
        let sysGain = self.systemGain
        var mixedBuffer = [Float](repeating: 0, count: frames * channels)

        if let buf = stereoBuffer {
            for i in 0..<(frames * channels) {
                mixedBuffer[i] = buf[i] * sysGain
            }
        } else {
            for i in 0..<(frames * channels) {
                mixedBuffer[i] = sysPtr[i] * sysGain
            }
        }

        // Read mic audio and mix in
        os_unfair_lock_lock(&micBufferLock)
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
        os_unfair_lock_unlock(&micBufferLock)

        // Hard clip
        for i in 0..<mixedBuffer.count {
            mixedBuffer[i] = max(-1.0, min(1.0, mixedBuffer[i]))
        }

        // Write to shared ring buffer
        mixedBuffer.withUnsafeBufferPointer { bufPtr in
            if let base = bufPtr.baseAddress {
                ringBufferWriter.write(frames: base, frameCount: finalFrameCount)
            }
        }
    }

    deinit {
        stop()
    }
}
