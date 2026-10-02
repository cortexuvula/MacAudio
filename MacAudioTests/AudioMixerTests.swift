import XCTest
import CoreAudio
@testable import MacAudio

/// Mic mock: start() fails for devices listed in failStartForDevices so the
/// rollback path can be exercised deterministically; every setInputDevice and
/// start attempt is recorded in order.
private final class MicCaptureMock: MicCapturing {
    var deviceID: AudioDeviceID = kAudioObjectUnknown
    var sampleRate: Float64 = 0
    var onAudioReceived: ((UnsafePointer<Float>, UInt32, UInt32) -> Void)?
    var failStartForDevices: Set<AudioDeviceID> = []
    private(set) var setInputDeviceCalls: [AudioDeviceID] = []
    private(set) var startAttemptDevices: [AudioDeviceID] = []
    private(set) var stopCallCount = 0

    func setInputDevice(_ id: AudioDeviceID) {
        deviceID = id
        setInputDeviceCalls.append(id)
    }

    func start() throws {
        startAttemptDevices.append(deviceID)
        if failStartForDevices.contains(deviceID) {
            throw NSError(domain: "MacAudioTests", code: 1,
                          userInfo: [NSLocalizedDescriptionKey: "mock mic failed to start"])
        }
    }

    func stop() {
        stopCallCount += 1
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

    func open() throws {
        openCallCount += 1
    }

    func write(frames: UnsafePointer<Float>, frameCount: UInt32) {
        writtenFrameCounts.append(frameCount)
    }

    func close() {
        closeCallCount += 1
    }

    func destroy() {
        destroyCallCount += 1
    }
}

final class AudioMixerMicDeviceSwitchTests: XCTestCase {

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

        let setInputCallsAfterStart = mic.setInputDeviceCalls.count
        let startAttemptsAfterStart = mic.startAttemptDevices.count

        mic.failStartForDevices.insert(100)
        let switchError = mixer.setMicDevice(100)

        XCTAssertEqual(switchError, "mock mic failed to start",
                       "the new device's start failure must be reported to the caller")
        XCTAssertEqual(mic.stopCallCount, 1,
                       "the mic IOProc must be cycled before attempting the switch")
        XCTAssertEqual(Array(mic.setInputDeviceCalls.dropFirst(setInputCallsAfterStart)), [100, 50],
                       "the new device must be attempted, then the previous device restored")
        XCTAssertEqual(Array(mic.startAttemptDevices.dropFirst(startAttemptsAfterStart)), [100, 50],
                       "start must fail on the new device, then succeed on the restored one")
        XCTAssertEqual(mic.deviceID, 50,
                       "capture must end up on the previous device")
        XCTAssertEqual(ring.closeCallCount, 0,
                       "a live switch must not close the ring buffer writer")
        XCTAssertTrue(mixer.isRunning, "the mixer must keep running after a failed switch")
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
    }
}
