import XCTest
import CoreAudio
@testable import MacAudio

/// Mic mock: start() fails for devices listed in failStartForDevices so the
/// rollback path can be exercised deterministically; every setInputDevice,
/// start attempt, and stop is recorded in a single ordered event log so
/// tests can assert call ordering, not just counts.
private final class MicCaptureMock: MicCapturing {
    var deviceID: AudioDeviceID = kAudioObjectUnknown
    var sampleRate: Float64 = 0
    var onAudioReceived: ((UnsafePointer<Float>, UInt32, UInt32) -> Void)?
    var failStartForDevices: Set<AudioDeviceID> = []
    private(set) var events: [String] = []

    func setInputDevice(_ id: AudioDeviceID) {
        deviceID = id
        events.append("setInput \(id)")
    }

    func start() throws {
        events.append("start \(deviceID)")
        if failStartForDevices.contains(deviceID) {
            throw NSError(domain: "MacAudioTests", code: 1,
                          userInfo: [NSLocalizedDescriptionKey: "mock mic failed to start"])
        }
    }

    func stop() {
        events.append("stop")
    }
}

/// System capture mock whose start() always fails, forcing AudioMixer into
/// the mic-only path (a real tap depends on Screen Recording TCC state).
private final class SystemAudioCaptureMock: SystemAudioCapturing {
    var onAudioReceived: ((UnsafePointer<AudioBufferList>, UInt32) -> Void)?
    var sampleRate: Float64 = 0
    private(set) var stopCallCount = 0

    func start() throws {
        throw NSError(domain: "MacAudioTests", code: 2,
                      userInfo: [NSLocalizedDescriptionKey: "mock system capture unavailable"])
    }

    func stop() {
        stopCallCount += 1
    }
}

private final class RingBufferWriterMock: RingBufferWriting {
    private(set) var openCallCount = 0
    private(set) var closeCallCount = 0
    private(set) var destroyCallCount = 0
    private(set) var writtenFrameCounts: [UInt32] = []
    /// Interleaved sample payload of each write, so tests can pin delivered
    /// contents (gain/clip), not just frame counts.
    private(set) var writtenSamples: [[Float]] = []
    /// When non-nil, open() throws it (models writer-ownership refusal from
    /// a second app instance).
    var stubbedOpenError: Error?

    func open() throws {
        if let stubbedOpenError { throw stubbedOpenError }
        openCallCount += 1
    }

    func write(frames: UnsafePointer<Float>, frameCount: UInt32) {
        writtenFrameCounts.append(frameCount)
        let sampleCount = Int(frameCount) * Int(AudioConstants.numChannels)
        writtenSamples.append(Array(UnsafeBufferPointer(start: frames, count: sampleCount)))
    }

    func close() {
        closeCallCount += 1
    }

    func destroy() {
        destroyCallCount += 1
    }
}

final class AudioMixerMicDeviceSwitchTests: XCTestCase {

    /// A refused writer open (second app instance holds writer ownership)
    /// must fail start() outright — the refusal error propagates and no
    /// capture machinery is left half-started.
    func test_start_propagates_writer_open_refusal_without_starting_mic() {
        let mic = MicCaptureMock()
        mic.deviceID = 50
        let ring = RingBufferWriterMock()
        ring.stubbedOpenError = NSError(
            domain: "MacAudio", code: 2,
            userInfo: [NSLocalizedDescriptionKey: "Another MacAudio instance is already capturing audio"])
        let mixer = AudioMixer(micCapture: mic,
                               systemCapture: SystemAudioCaptureMock(),
                               ringBufferWriter: ring)

        XCTAssertThrowsError(try mixer.start(micDeviceID: 50)) { error in
            XCTAssertEqual((error as NSError).localizedDescription,
                           "Another MacAudio instance is already capturing audio",
                           "the writer-ownership refusal must surface verbatim")
        }
        XCTAssertFalse(mixer.isRunning,
                       "a refused writer open must not leave a running mixer")
        XCTAssertTrue(mic.events.isEmpty,
                      "mic capture must not have been started when the writer open was refused")
        XCTAssertEqual(ring.closeCallCount, 0,
                       "nothing was opened, so no stop-side close may run")
    }

    func test_setMicDevice_returns_error_and_restores_previous_device_when_running() throws {
        let mic = MicCaptureMock()
        mic.deviceID = 50
        mic.sampleRate = 48000
        let ring = RingBufferWriterMock()
        let mixer = AudioMixer(micCapture: mic,
                               systemCapture: SystemAudioCaptureMock(),
                               ringBufferWriter: ring)
        try mixer.start(micDeviceID: 50)
        XCTAssertTrue(mixer.isRunning, "test requires a running mixer")
        XCTAssertFalse(mixer.systemCaptureActive, "test requires the mic-only path")

        let eventsAfterStart = mic.events.count

        mic.failStartForDevices.insert(100)
        let switchError = mixer.setMicDevice(100)

        XCTAssertEqual(switchError, "mock mic failed to start",
                       "the new device's start failure must be reported to the caller")
        // A single ordered log pins the sequence: the IOProc is stopped
        // BEFORE the device is swapped (a regression that sets the device
        // first would log "setInput 100" before "stop" and fail here), then
        // the new device is attempted and the previous device restored.
        XCTAssertEqual(Array(mic.events.dropFirst(eventsAfterStart)),
                       ["stop", "setInput 100", "start 100", "setInput 50", "start 50"],
                       "stop must precede setInput; the failed switch must roll back to the previous device")
        XCTAssertEqual(mic.deviceID, 50,
                       "capture must end up on the previous device")
        XCTAssertEqual(ring.closeCallCount, 0,
                       "a live switch must not close the ring buffer writer")
        XCTAssertTrue(mixer.isRunning, "the mixer must keep running after a failed switch")
    }

    func test_setMicDevice_reports_distinct_error_and_stops_mixer_when_restore_also_fails() throws {
        let mic = MicCaptureMock()
        mic.deviceID = 50
        mic.sampleRate = 48000
        let system = SystemAudioCaptureMock()
        let ring = RingBufferWriterMock()
        let mixer = AudioMixer(micCapture: mic,
                               systemCapture: system,
                               ringBufferWriter: ring)
        try mixer.start(micDeviceID: 50)
        XCTAssertTrue(mixer.isRunning, "test requires a running mixer")

        let eventsAfterStart = mic.events.count

        // Both the switch (100) and the restore (50) fail: the mic IOProc
        // ends up dead on both devices.
        mic.failStartForDevices.insert(100)
        mic.failStartForDevices.insert(50)
        let switchError = mixer.setMicDevice(100)

        XCTAssertEqual(switchError,
                       "Mic capture failed after device switch (mock mic failed to start); previous device 50 could not be restarted — mixer stopped",
                       "the double failure must surface a distinct error naming both failures")
        XCTAssertFalse(mixer.isRunning,
                       "a dead mic IOProc on both devices must not leave a \"running\" mixer capturing silence")
        XCTAssertEqual(Array(mic.events.dropFirst(eventsAfterStart)),
                       ["stop", "setInput 100", "start 100", "setInput 50", "start 50", "stop"],
                       "the emergency stop must stop the mic capture after the failed restore")
        XCTAssertEqual(system.stopCallCount, 1,
                       "stopping the mixer must stop system capture")
        XCTAssertEqual(ring.closeCallCount, 1,
                       "stopping the mixer must close the ring buffer writer")
    }

    func test_setMicDevice_when_not_running_only_sets_device() throws {
        let mic = MicCaptureMock()
        mic.deviceID = 50
        mic.sampleRate = 48000
        let ring = RingBufferWriterMock()
        let mixer = AudioMixer(micCapture: mic,
                               systemCapture: SystemAudioCaptureMock(),
                               ringBufferWriter: ring)
        XCTAssertFalse(mixer.isRunning, "test requires a mixer that was never started")

        let switchError = mixer.setMicDevice(100)

        XCTAssertNil(switchError, "setting the device while stopped must succeed")
        XCTAssertEqual(mic.events, ["setInput 100"],
                       "while not running, only the device is set — no stop/start cycle")
        XCTAssertEqual(mic.deviceID, 100,
                       "the device must be recorded for the next start()")
        XCTAssertEqual(ring.openCallCount, 0,
                       "setting the device while stopped must not touch the ring buffer writer")
    }

    func test_setMicDevice_is_noop_for_current_device_while_running() throws {
        let mic = MicCaptureMock()
        mic.deviceID = 50
        mic.sampleRate = 48000
        let ring = RingBufferWriterMock()
        let mixer = AudioMixer(micCapture: mic,
                               systemCapture: SystemAudioCaptureMock(),
                               ringBufferWriter: ring)
        try mixer.start(micDeviceID: 50)
        XCTAssertTrue(mixer.isRunning, "test requires a running mixer")

        let eventsAfterStart = mic.events.count

        let switchError = mixer.setMicDevice(50)

        XCTAssertNil(switchError, "re-selecting the current device must succeed")
        XCTAssertEqual(mic.events.count, eventsAfterStart,
                       "re-selecting the running device must not cycle the IOProc")
        XCTAssertTrue(mixer.isRunning, "the mixer must be untouched")
        XCTAssertEqual(ring.closeCallCount, 0,
                       "re-selecting the running device must not close the ring buffer writer")
    }

    func test_mic_only_delivery_writes_resampled_frame_count_to_ring() throws {
        let mic = MicCaptureMock()
        mic.deviceID = 50
        // 44.1k mic forces AudioMixer to build a 44.1k -> 48k converter.
        mic.sampleRate = 44100
        let ring = RingBufferWriterMock()
        let mixer = AudioMixer(micCapture: mic,
                               systemCapture: SystemAudioCaptureMock(),
                               ringBufferWriter: ring)
        try mixer.start(micDeviceID: 50)
        XCTAssertTrue(mixer.isRunning, "test requires a running mixer")
        XCTAssertFalse(mixer.systemCaptureActive,
                       "mic-only mode writes straight to the ring writer")

        let frameCount: UInt32 = 1024
        let samples = [Float](repeating: 0.2, count: Int(frameCount) * 2)
        samples.withUnsafeBufferPointer { buf in
            mic.onAudioReceived?(buf.baseAddress!, frameCount, 2)
        }

        XCTAssertEqual(ring.writtenFrameCounts.count, 1,
                       "the mic callback must produce exactly one ring write")
        let written = try XCTUnwrap(ring.writtenFrameCounts.first)

        // Reference: the same 44.1k -> 48k conversion of the same input.
        let reference = try XCTUnwrap(SampleRateConverter(sourceRate: 44100, destRate: 48000))
        let referenceCapacity = UInt32(frameCount) * 4
        var referenceOut = [Float](repeating: 0, count: Int(referenceCapacity))
        let referenceFrames = samples.withUnsafeBufferPointer { buf -> UInt32 in
            referenceOut.withUnsafeMutableBufferPointer { out in
                reference.convert(buf.baseAddress!, frameCount: frameCount,
                                  into: out.baseAddress!, outputCapacity: referenceCapacity)
            }
        }
        XCTAssertGreaterThan(referenceFrames, 0, "reference conversion must produce output")

        XCTAssertEqual(written, referenceFrames,
                       "ring write frameCount must be the resampled count, not the pre-resample input count")
        XCTAssertNotEqual(written, frameCount,
                          "writing the unresampled input count is the regression this pins")
        // 44.1k -> 48k is a 160/147 upsample; a fresh converter may sit ~2%
        // under the naive ratio (filter boundary), so allow generous slack —
        // but far less than the ~9% gap between resampled and unresampled.
        let expected = Double(frameCount) * 48000.0 / 44100.0
        XCTAssertGreaterThan(Double(written), expected - 48,
                             "resampled count must track the 44.1k -> 48k ratio")
        XCTAssertLessThan(Double(written), expected + 48,
                          "resampled count must track the 44.1k -> 48k ratio")

        // Contents: each delivered sample must be the reference conversion
        // with the default mic gain (0.7) applied and hard-clipped. The
        // converter is deterministic across instances, so this is exact
        // modulo float slack; the un-gained resampled signal (≈0.2) sits
        // 0.06 away, far outside the tolerance.
        let delivered = try XCTUnwrap(ring.writtenSamples.first)
        XCTAssertEqual(delivered.count, Int(written) * Int(AudioConstants.numChannels),
                       "delivered sample count must match the written frame count")
        for (i, sample) in delivered.enumerated() {
            let gained = max(-1.0, min(1.0, referenceOut[i] * AudioConstants.defaultGain))
            XCTAssertEqual(sample, gained, accuracy: 0.005,
                           "delivered sample \(i) must be the resampled input with defaultGain applied and clipped")
            // The resampler's ripple on a constant 0.2 input is ~±0.015, so
            // every sample must sit near 0.2 × 0.7 = 0.14 — and clearly not
            // near the un-gained 0.2.
            XCTAssertLessThan(abs(sample - 0.2 * AudioConstants.defaultGain), 0.04,
                              "delivered sample \(i) must be ≈ input × defaultGain")
        }
    }
}
