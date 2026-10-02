import AVFAudio
import CoreAudio
import os

/// Seam over AudioMixer covering the members AppState drives a mixer through,
/// so the menu state machine can be tested against a mock.
protocol AudioMixing: AnyObject {
    var micGain: Float { get set }
    var systemGain: Float { get set }
    func start(micDeviceID: AudioDeviceID?) throws
    func stop()
    func setMicDevice(_ deviceID: AudioDeviceID) -> String?
    func destroySharedMemory()
}

/// Seam over SystemAudioCapture so tests can force the mic-only path
/// deterministically (a real tap depends on Screen Recording TCC state).
protocol SystemAudioCapturing: AnyObject {
    var onAudioReceived: ((UnsafePointer<AudioBufferList>, UInt32) -> Void)? { get set }
    var sampleRate: Float64 { get }
    func start() throws
    func stop()
}

extension SystemAudioCapture: SystemAudioCapturing {}

final class AudioMixer: AudioMixing {
    private let micCapture: MicCapturing
    private let systemCapture: SystemAudioCapturing
    private let ringBufferWriter: RingBufferWriting
    private let logger = Logger(subsystem: "com.macaudio.app", category: "mixer")

    /// Production callers use the default arguments (the real capture and
    /// writer instances); tests inject mocks.
    init(micCapture: MicCapturing = MicCapture(),
         systemCapture: SystemAudioCapturing = SystemAudioCapture(),
         ringBufferWriter: RingBufferWriting = SharedRingBufferWriter()) {
        self.micCapture = micCapture
        self.systemCapture = systemCapture
        self.ringBufferWriter = ringBufferWriter
    }

    // Lock-free mic buffer for cross-thread communication
    private var micBufferLock = os_unfair_lock()
    private var micBuffer = [Float](repeating: 0, count: Int(AudioConstants.ringBufferFrames) * Int(AudioConstants.numChannels))
    private var micWritePos: Int = 0
    private var micReadPos: Int = 0

    // Guards the converter Optional pointers below between the main thread
    // (start/stop/live device switch) and the IO threads that load them in
    // the delivery paths. Held only for the pointer load/store — never
    // across convert(), which runs on a strong local so a concurrent swap
    // can't free the instance mid-call. Deliberately separate from
    // micBufferLock, which is held across the long buffering/mixing paths.
    private var converterLock = os_unfair_lock()
    private var micConverter: SampleRateConverter?
    private var systemConverter: SampleRateConverter?

    // Preallocated scratch pools for the realtime callbacks, sized to the
    // worst case. Steady-state callbacks reuse them without any heap
    // allocation; ensureScratchCapacity regrows only if a callback ever
    // exceeds that (an IO cycle larger than the whole ring). Each pool is
    // touched only by the single IO thread driving its source.
    //
    // The stereo pools only ever hold pre-resampling frames, so a full ring
    // of interleaved stereo (AudioConstants.maxFrameSize) covers them. The
    // resample/output pools can hold converter output, whose worst case is a
    // full ring upsampled 44.1k→48k — the largest supported ratio — which
    // EXCEEDS maxFrameSize; maxResampledSize covers it so the first full
    // 44.1k callback doesn't allocate on the realtime thread.
    private static let maxResampledFrames = Int(ceil(Double(AudioConstants.ringBufferFrames) * 48000.0 / 44100.0)) + 1
    private static let maxResampledSize = maxResampledFrames * Int(AudioConstants.numChannels)
    private var micStereoBuffer = [Float](repeating: 0, count: AudioConstants.maxFrameSize)
    private var micResampledBuffer = [Float](repeating: 0, count: AudioMixer.maxResampledSize)
    private var micOutputBuffer = [Float](repeating: 0, count: AudioMixer.maxResampledSize)
    private var systemStereoBuffer = [Float](repeating: 0, count: AudioConstants.maxFrameSize)
    private var systemResampledBuffer = [Float](repeating: 0, count: AudioMixer.maxResampledSize)
    private var systemMixedBuffer = [Float](repeating: 0, count: AudioMixer.maxResampledSize)

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

        // From here on, any failure must close the ring buffer so we don't leak
        // the SHM mapping or leave the writer half-open.
        var didStartMic = false
        do {
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
            didStartMic = true
            logger.info("Mic capture started")

            rebuildMicConverter()

        do {
            try systemCapture.start()
            systemCaptureActive = true
            logger.info("System audio capture started")
            systemCaptureError = nil

            // Create system audio sample rate converter if needed. Build it
            // outside converterLock (AVAudioConverter allocation is slow),
            // then swap the pointer under it.
            let destRate = AudioConstants.defaultSampleRate
            let newSystemConverter: SampleRateConverter?
            if systemCapture.sampleRate > 0 && systemCapture.sampleRate != destRate {
                newSystemConverter = SampleRateConverter(sourceRate: systemCapture.sampleRate, destRate: destRate)
                logger.info("System SRC: \(self.systemCapture.sampleRate)Hz -> \(destRate)Hz")
            } else {
                newSystemConverter = nil
            }
            os_unfair_lock_lock(&converterLock)
            systemConverter = newSystemConverter
            os_unfair_lock_unlock(&converterLock)
        } catch {
            logger.error("System audio capture failed: \(error.localizedDescription)")
            systemCaptureActive = false
            systemCaptureError = error.localizedDescription
            os_unfair_lock_lock(&converterLock)
            systemConverter = nil
            os_unfair_lock_unlock(&converterLock)
            // Continue with mic only
        }

            isRunning = true
        } catch {
            logger.error("AudioMixer.start failed: \(error.localizedDescription)")
            if didStartMic { micCapture.stop() }
            ringBufferWriter.close()
            throw error
        }
    }

    func stop() {
        guard isRunning else { return }
        isRunning = false
        systemCaptureActive = false

        micCapture.stop()
        systemCapture.stop()
        os_unfair_lock_lock(&converterLock)
        micConverter = nil
        systemConverter = nil
        os_unfair_lock_unlock(&converterLock)
        ringBufferWriter.close()
        logger.info("Audio mixer stopped")
    }

    func destroySharedMemory() {
        ringBufferWriter.destroy()
    }

    /// Returns nil on success, or an error message if a live switch failed
    /// (the previously running device is restored in that case). If the
    /// restore of the previous device also fails, the mixer stops itself —
    /// the mic IOProc is dead on both devices and a "running" mixer would
    /// capture silence — and the returned error reports the double failure.
    ///
    /// When capture is running, the mic IOProc is cycled (stop → set device →
    /// start) so the new device takes effect immediately; only the mic capture
    /// is restarted — the ring buffer writer stays open and the shm mapping is
    /// never torn down, and system capture, if active, keeps running. Runs on
    /// the caller's thread (menu/MainActor), never the IO thread.
    func setMicDevice(_ deviceID: AudioDeviceID) -> String? {
        guard isRunning else {
            micCapture.setInputDevice(deviceID)
            return nil
        }
        guard deviceID != micCapture.deviceID else { return nil }

        let previousDeviceID = micCapture.deviceID

        micCapture.stop()
        // Discard mic audio still buffered from the old device so it doesn't
        // drain into the mix after the switch.
        os_unfair_lock_lock(&micBufferLock)
        micReadPos = micWritePos
        os_unfair_lock_unlock(&micBufferLock)

        micCapture.setInputDevice(deviceID)
        do {
            try micCapture.start()
            rebuildMicConverter()
            logger.info("Mic device switched while running: \(previousDeviceID) -> \(deviceID)")
            return nil
        } catch {
            let switchErrorMessage = error.localizedDescription
            logger.error("Mic switch to device \(deviceID) failed: \(switchErrorMessage), restoring device \(previousDeviceID)")
            micCapture.setInputDevice(previousDeviceID)
            do {
                try micCapture.start()
                rebuildMicConverter()
            } catch {
                logger.error("Mic restore to device \(previousDeviceID) failed: \(error.localizedDescription); stopping mixer")
                // Both the switch and the restore failed, so the mic IOProc is
                // dead on both devices. Stop the mixer so the UI is honest
                // about capture being down instead of showing a "running"
                // mixer that captures silence.
                stop()
                return "Mic capture failed after device switch (\(switchErrorMessage)); previous device \(previousDeviceID) could not be restarted — mixer stopped"
            }
            return switchErrorMessage
        }
    }

    /// Recreates the mic SRC for the capture's current sample rate. Called
    /// from start() and after a live device switch — never from the IO thread.
    /// The new converter is built outside converterLock (AVAudioConverter
    /// allocation is slow); only the pointer swap takes the lock.
    private func rebuildMicConverter() {
        let destRate = AudioConstants.defaultSampleRate
        let newConverter: SampleRateConverter?
        if micCapture.sampleRate > 0 && micCapture.sampleRate != destRate {
            newConverter = SampleRateConverter(sourceRate: micCapture.sampleRate, destRate: destRate)
            logger.info("Mic SRC: \(self.micCapture.sampleRate)Hz -> \(destRate)Hz")
        } else {
            newConverter = nil
        }
        os_unfair_lock_lock(&converterLock)
        micConverter = newConverter
        os_unfair_lock_unlock(&converterLock)
    }

    // MARK: - Audio Callbacks

    /// Regrows a scratch pool when a callback's required capacity exceeds it.
    /// Contents are scratch — nothing is preserved across callbacks.
    private func ensureScratchCapacity(_ buffer: inout [Float], _ needed: Int) {
        if needed > buffer.count {
            buffer = [Float](repeating: 0, count: needed)
        }
    }

    private func handleMicAudio(_ floatPtr: UnsafePointer<Float>, frameCount: UInt32, channelCount: Int) {
        let frames = Int(frameCount)
        guard frames > 0 else { return }
        let outChannels = Int(AudioConstants.numChannels)

        // Step 1: Upmix/downmix to interleaved stereo in the reusable scratch
        // buffer when the channel count differs; use the input pointer
        // directly when it already matches.
        if channelCount == outChannels {
            micResampleAndDeliver(floatPtr, frames)
            return
        }

        ensureScratchCapacity(&micStereoBuffer, frames * outChannels)
        micStereoBuffer.withUnsafeMutableBufferPointer { stereo in
            guard let stereoBase = stereo.baseAddress else { return }
            if channelCount < outChannels {
                // Mono -> stereo upmix
                for frame in 0..<frames {
                    let sample = floatPtr[frame]
                    stereoBase[frame * outChannels] = sample
                    stereoBase[frame * outChannels + 1] = sample
                }
            } else {
                // Downmix to stereo (take first 2 channels)
                for frame in 0..<frames {
                    for ch in 0..<outChannels {
                        stereoBase[frame * outChannels + ch] = floatPtr[frame * channelCount + ch]
                    }
                }
            }
            micResampleAndDeliver(stereoBase, frames)
        }
    }

    /// Step 2 of the mic path: resample to the destination rate (if needed).
    /// `ptr` is only read for the duration of this call.
    private func micResampleAndDeliver(_ ptr: UnsafePointer<Float>, _ frames: Int) {
        let outChannels = Int(AudioConstants.numChannels)

        // Load the pointer under converterLock, then convert on the local
        // strong ref — the main thread may swap micConverter concurrently
        // (live device switch). Never hold the lock across convert().
        os_unfair_lock_lock(&converterLock)
        let loadedConverter = micConverter
        os_unfair_lock_unlock(&converterLock)

        guard let converter = loadedConverter else {
            micDeliver(ptr, frames)
            return
        }

        // Convert into a reusable scratch buffer so no internal pointer
        // escapes the converter. Capacity for the worst-case upsample of this
        // input.
        let ratio = converter.outputRate / converter.inputRate
        let maxOutFrames = Int(ceil(Double(frames) * ratio)) + 1
        ensureScratchCapacity(&micResampledBuffer, maxOutFrames * outChannels)
        let written = micResampledBuffer.withUnsafeMutableBufferPointer { out -> UInt32 in
            converter.convert(ptr, frameCount: UInt32(frames),
                              into: out.baseAddress!,
                              outputCapacity: UInt32(maxOutFrames * outChannels))
        }
        if written > 0 {
            micResampledBuffer.withUnsafeBufferPointer { out in
                micDeliver(out.baseAddress!, Int(written))
            }
        } else {
            // Conversion produced nothing — deliver the pre-conversion audio
            // rather than dropping the callback.
            micDeliver(ptr, frames)
        }
    }

    /// Step 3 of the mic path: buffer for mixing in the system callback, or
    /// gain + clip + write in mic-only mode.
    private func micDeliver(_ ptr: UnsafePointer<Float>, _ frameCount: Int) {
        let outChannels = Int(AudioConstants.numChannels)

        if systemCaptureActive {
            // Buffer mic audio for mixing in the system audio callback
            let bufferCapacity = micBuffer.count

            os_unfair_lock_lock(&micBufferLock)
            defer { os_unfair_lock_unlock(&micBufferLock) }

            for frame in 0..<frameCount {
                let writeIdx = (micWritePos + frame * outChannels) % bufferCapacity
                for ch in 0..<outChannels {
                    micBuffer[(writeIdx + ch) % bufferCapacity] = ptr[frame * outChannels + ch]
                }
            }
            micWritePos = (micWritePos + frameCount * outChannels) % bufferCapacity
        } else {
            // Mic-only mode: apply gain, clip, and write directly to ring buffer
            let gain = self.micGain

            ensureScratchCapacity(&micOutputBuffer, frameCount * outChannels)
            micOutputBuffer.withUnsafeMutableBufferPointer { out in
                guard let outBase = out.baseAddress else { return }
                for i in 0..<(frameCount * outChannels) {
                    outBase[i] = max(-1.0, min(1.0, ptr[i] * gain))
                }
                self.ringBufferWriter.write(frames: outBase, frameCount: UInt32(frameCount))
            }
        }
    }

    private func handleSystemAudio(_ bufferList: UnsafePointer<AudioBufferList>, frameCount: UInt32) {
        let channels = Int(AudioConstants.numChannels)
        let frames = Int(frameCount)
        guard frames > 0 else { return }

        // Extract system audio as interleaved stereo
        let numBuffers = Int(bufferList.pointee.mNumberBuffers)
        guard numBuffers >= 1 else { return }

        let firstBuffer = bufferList.pointee.mBuffers
        guard let data = firstBuffer.mData else { return }

        let sysFloats = data.assumingMemoryBound(to: Float.self)
        let sysChannels = Int(firstBuffer.mNumberChannels)

        // Upmix/downmix to interleaved stereo in the reusable scratch buffer
        // when the channel count differs; use the tap buffer directly when it
        // matches.
        if sysChannels == channels {
            systemResampleAndMix(UnsafePointer(sysFloats), frames)
            return
        }

        ensureScratchCapacity(&systemStereoBuffer, frames * channels)
        systemStereoBuffer.withUnsafeMutableBufferPointer { stereo in
            guard let stereoBase = stereo.baseAddress else { return }
            if sysChannels < channels {
                // Mono -> stereo upmix
                for frame in 0..<frames {
                    let sample = sysFloats[frame]
                    stereoBase[frame * channels] = sample
                    stereoBase[frame * channels + 1] = sample
                }
            } else {
                // Downmix: take first 2 channels
                for frame in 0..<frames {
                    for ch in 0..<channels {
                        stereoBase[frame * channels + ch] = sysFloats[frame * sysChannels + ch]
                    }
                }
            }
            systemResampleAndMix(stereoBase, frames)
        }
    }

    /// Resamples to the destination rate (if needed), then mixes and writes.
    /// `ptr` is only read for the duration of this call.
    private func systemResampleAndMix(_ ptr: UnsafePointer<Float>, _ frames: Int) {
        let channels = Int(AudioConstants.numChannels)

        // Load the pointer under converterLock, then convert on the local
        // strong ref — the main thread may swap systemConverter concurrently
        // (start/stop). Never hold the lock across convert().
        os_unfair_lock_lock(&converterLock)
        let loadedConverter = systemConverter
        os_unfair_lock_unlock(&converterLock)

        guard let converter = loadedConverter else {
            systemMixAndWrite(ptr, frames)
            return
        }

        // Convert into a reusable scratch buffer so no internal pointer
        // escapes the converter.
        let ratio = converter.outputRate / converter.inputRate
        let maxOutFrames = Int(ceil(Double(frames) * ratio)) + 1
        ensureScratchCapacity(&systemResampledBuffer, maxOutFrames * channels)
        let written = systemResampledBuffer.withUnsafeMutableBufferPointer { out -> UInt32 in
            converter.convert(ptr, frameCount: UInt32(frames),
                              into: out.baseAddress!,
                              outputCapacity: UInt32(maxOutFrames * channels))
        }
        if written > 0 {
            systemResampledBuffer.withUnsafeBufferPointer { out in
                systemMixAndWrite(out.baseAddress!, Int(written))
            }
        } else {
            // Conversion produced nothing — mix the pre-conversion audio
            // rather than dropping the callback.
            systemMixAndWrite(ptr, frames)
        }
    }

    /// Mixes system audio with the buffered mic audio, hard-clips, and writes
    /// to the shared ring buffer.
    private func systemMixAndWrite(_ sysPtr: UnsafePointer<Float>, _ frameCount: Int) {
        let channels = Int(AudioConstants.numChannels)

        // Build mixing buffer with system audio + gain
        let sysGain = self.systemGain
        ensureScratchCapacity(&systemMixedBuffer, frameCount * channels)

        systemMixedBuffer.withUnsafeMutableBufferPointer { mixed in
            guard let mixedBase = mixed.baseAddress else { return }
            for i in 0..<(frameCount * channels) {
                mixedBase[i] = sysPtr[i] * sysGain
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
            let samplesToRead = min(available, frameCount * channels)

            for i in 0..<samplesToRead {
                let readIdx = (micReadPos + i) % bufferCapacity
                mixedBase[i] += micBuffer[readIdx] * micGainVal
            }
            micReadPos = (micReadPos + samplesToRead) % bufferCapacity
            os_unfair_lock_unlock(&micBufferLock)

            // Hard clip
            for i in 0..<(frameCount * channels) {
                mixedBase[i] = max(-1.0, min(1.0, mixedBase[i]))
            }

            // Write to shared ring buffer
            self.ringBufferWriter.write(frames: mixedBase, frameCount: UInt32(frameCount))
        }
    }

    deinit {
        stop()
    }
}
