import XCTest
import CoreAudio
@testable import MacAudio

/// Mock behind the AudioMixing seam: records setMicDevice traffic and
/// returns a canned result so AppState's revert path can be driven
/// deterministically.
private final class MockAudioMixer: AudioMixing {
    var micGain: Float = AudioConstants.defaultGain
    var systemGain: Float = AudioConstants.defaultGain
    private(set) var setMicDeviceCalls: [AudioDeviceID] = []
    private(set) var stopCallCount = 0
    private(set) var destroySharedMemoryCallCount = 0
    /// When non-nil, setMicDevice reports a live-switch failure with this message.
    var stubbedSwitchError: String?
    /// What the mixer reports after a stubbed failure. Defaults to true — a
    /// single-failure switch leaves the mixer running; set false to simulate
    /// the double-failure emergency stop where AudioMixer stops itself.
    var isRunning = true

    func start(micDeviceID: AudioDeviceID?) throws {
        isRunning = true
    }

    func stop() {
        isRunning = false
        stopCallCount += 1
    }

    func setMicDevice(_ deviceID: AudioDeviceID) -> String? {
        setMicDeviceCalls.append(deviceID)
        return stubbedSwitchError
    }

    func destroySharedMemory() {
        destroySharedMemoryCallCount += 1
    }
}

/// updateMicDevice is a revert state machine: when the mixer reports a failed
/// live switch (having restored the previous device itself), the menu must
/// point back at the device that is actually capturing; on success the new
/// selection sticks and lastError is untouched. A double failure (the restore
/// also threw) leaves the mixer stopped, and the menu must say so.
final class AppStateMicDeviceSwitchTests: XCTestCase {
    private let oldDevice = AudioDevice(id: 100, name: "Old Mic", uid: "old-mic-uid")
    private let newDevice = AudioDevice(id: 200, name: "New Mic", uid: "new-mic-uid")

    /// Name of this test's isolated defaults suite, removed in tearDown.
    private var suiteName: String?

    override func tearDown() {
        if let suiteName {
            UserDefaults.standard.removePersistentDomain(forName: suiteName)
        }
        suiteName = nil
        super.tearDown()
    }

    /// AppState persists via the injected UserDefaults; tests run against a
    /// freshly created suite so the test host's real preferences are never
    /// read or written (a crash mid-test can't leave them mutated either).
    private func makeIsolatedDefaults() throws -> UserDefaults {
        let name = "AppStateMicDeviceSwitchTests-\(UUID().uuidString)"
        suiteName = name
        return try XCTUnwrap(UserDefaults(suiteName: name),
                             "a fresh suite name must always initialize")
    }

    @MainActor
    func test_updateMicDevice_reverts_selection_and_uid_and_sets_error_when_switch_fails() throws {
        let mixer = MockAudioMixer()
        mixer.stubbedSwitchError = "switch failed"
        let defaults = try makeIsolatedDefaults()
        let state = AppState(audioMixer: mixer, defaults: defaults)
        state.availableMicDevices = [oldDevice, newDevice]
        state.selectedMicDeviceID = oldDevice.id
        state.preferredMicUID = oldDevice.uid
        state.lastError = nil
        // Capture is live; the mock defaults to isRunning == true, modeling a
        // single failure where the mixer restored the previous device.
        state.isActive = true
        state.captureStatus = .micOnly

        state.updateMicDevice(newDevice.id)

        XCTAssertEqual(mixer.setMicDeviceCalls, [newDevice.id],
                       "the mixer must be asked to switch to the new device")
        XCTAssertEqual(state.selectedMicDeviceID, oldDevice.id,
                       "selection must revert to the device that is still capturing")
        XCTAssertEqual(state.preferredMicUID, oldDevice.uid,
                       "UID preference must revert to the device that is still capturing")
        XCTAssertEqual(state.lastError, "switch failed",
                       "the switch failure must surface to the user")
        XCTAssertTrue(state.isActive,
                      "a single-failure switch keeps capture live")
        XCTAssertEqual(state.captureStatus, .micOnly,
                       "a single-failure switch keeps the capture status untouched")
        XCTAssertEqual(defaults.integer(forKey: "selectedMicDeviceID"), Int(oldDevice.id),
                       "savePreferences must persist the reverted device selection")
        XCTAssertEqual(defaults.string(forKey: "selectedMicDeviceUID"), oldDevice.uid,
                       "savePreferences must persist the reverted device UID preference")
    }

    /// Double failure: the switch AND the restore start() threw, so the mixer
    /// emergency-stopped itself (isRunning == false). Beyond the usual revert,
    /// the menu must stop claiming capture is live.
    @MainActor
    func test_updateMicDevice_marks_capture_stopped_when_double_failure_stops_mixer() throws {
        let mixer = MockAudioMixer()
        mixer.stubbedSwitchError = "switch and restore failed — mixer stopped"
        mixer.isRunning = false
        let defaults = try makeIsolatedDefaults()
        let state = AppState(audioMixer: mixer, defaults: defaults)
        state.availableMicDevices = [oldDevice, newDevice]
        state.selectedMicDeviceID = oldDevice.id
        state.preferredMicUID = oldDevice.uid
        state.lastError = nil
        state.isActive = true
        state.captureStatus = .both

        state.updateMicDevice(newDevice.id)

        XCTAssertEqual(mixer.setMicDeviceCalls, [newDevice.id],
                       "the mixer must be asked to switch to the new device")
        XCTAssertEqual(state.selectedMicDeviceID, oldDevice.id,
                       "selection must still revert to the previous device")
        XCTAssertEqual(state.preferredMicUID, oldDevice.uid,
                       "UID preference must still revert to the previous device")
        XCTAssertEqual(state.lastError, "switch and restore failed — mixer stopped",
                       "the double failure must surface to the user")
        XCTAssertFalse(state.isActive,
                       "the menu must not claim capture is live after the mixer's emergency stop")
        XCTAssertEqual(state.captureStatus, .stopped,
                       "capture status must reflect the mixer's stopped state")
    }

    /// A double failure must not orphan the ring buffer's shared-memory
    /// segment: the stopped mixer stays installed in AppState, so quitting
    /// without restarting still routes termination cleanup through it
    /// (nil-assigning the mixer here would drop the destroySharedMemory call
    /// and leak the segment).
    @MainActor
    func test_updateMicDevice_double_failure_still_destroys_shared_memory_on_termination() throws {
        let mixer = MockAudioMixer()
        mixer.stubbedSwitchError = "switch and restore failed — mixer stopped"
        mixer.isRunning = false
        let defaults = try makeIsolatedDefaults()
        let state = AppState(audioMixer: mixer, defaults: defaults)
        state.availableMicDevices = [oldDevice, newDevice]
        state.selectedMicDeviceID = oldDevice.id
        state.isActive = true

        state.updateMicDevice(newDevice.id)

        XCTAssertFalse(state.isActive,
                       "precondition: the double failure must have stopped capture")

        state.cleanupForTermination()

        XCTAssertEqual(mixer.destroySharedMemoryCallCount, 1,
                       "termination cleanup must reach the retained mixer's destroySharedMemory")
        XCTAssertEqual(mixer.stopCallCount, 1,
                       "termination cleanup must route through the retained mixer")
    }

    @MainActor
    func test_updateMicDevice_keeps_new_selection_and_leaves_last_error_untouched_on_success() throws {
        let mixer = MockAudioMixer()
        let defaults = try makeIsolatedDefaults()
        let state = AppState(audioMixer: mixer, defaults: defaults)
        state.availableMicDevices = [oldDevice, newDevice]
        state.selectedMicDeviceID = oldDevice.id
        state.preferredMicUID = oldDevice.uid
        state.lastError = "pre-existing error"

        state.updateMicDevice(newDevice.id)

        XCTAssertEqual(mixer.setMicDeviceCalls, [newDevice.id])
        XCTAssertEqual(state.selectedMicDeviceID, newDevice.id,
                       "a successful switch must keep the new selection")
        XCTAssertEqual(state.preferredMicUID, newDevice.uid,
                       "a successful switch must prefer the new device's UID")
        XCTAssertEqual(state.lastError, "pre-existing error",
                       "a successful switch must not touch lastError")
    }
}

/// stopAudio/teardown lifecycle (issue #3, R3): Stop parks the mixer without
/// destroying its shared-memory segment (a connected driver client's mapping
/// must survive Stop→Start), yet quitting after a Stop must still reach
/// destroySharedMemory through the retained mixer — and exactly once.
final class AppStateStopRetainsMixerTests: XCTestCase {

    /// Name of this test's isolated defaults suite, removed in tearDown.
    private var suiteName: String?

    override func tearDown() {
        if let suiteName {
            UserDefaults.standard.removePersistentDomain(forName: suiteName)
        }
        suiteName = nil
        super.tearDown()
    }

    private func makeIsolatedDefaults() throws -> UserDefaults {
        let name = "AppStateStopRetainsMixerTests-\(UUID().uuidString)"
        suiteName = name
        return try XCTUnwrap(UserDefaults(suiteName: name),
                             "a fresh suite name must always initialize")
    }

    /// User-toggle Stop must stop the mixer but never destroy the segment.
    @MainActor
    func test_stop_calls_stop_but_never_destroy() throws {
        let mixer = MockAudioMixer()
        let state = AppState(audioMixer: mixer, defaults: try makeIsolatedDefaults())
        state.isActive = true

        state.toggleActive()  // isActive == true routes to stopAudio()

        XCTAssertEqual(mixer.stopCallCount, 1,
                       "Stop must stop the mixer")
        XCTAssertEqual(mixer.destroySharedMemoryCallCount, 0,
                       "Stop must NOT destroy the shm segment — it survives Stop→Start")
        XCTAssertFalse(state.isActive)
        XCTAssertEqual(state.captureStatus, .stopped)
    }

    /// Quitting after a Stop must still destroy the segment: the stopped
    /// mixer stays reachable by cleanupForTermination (retention is the fix).
    @MainActor
    func test_stop_then_termination_reaches_destroy_via_retained_mixer() throws {
        let mixer = MockAudioMixer()
        let state = AppState(audioMixer: mixer, defaults: try makeIsolatedDefaults())
        state.isActive = true

        state.toggleActive()
        state.cleanupForTermination()

        XCTAssertEqual(mixer.destroySharedMemoryCallCount, 1,
                       "quitting after a Stop must still destroy the segment via the retained mixer")
    }

    /// Termination cleanup is idempotent: repeated calls must not repeat the
    /// destroy.
    @MainActor
    func test_repeated_termination_cleanup_does_not_repeat_destroy() throws {
        let mixer = MockAudioMixer()
        let state = AppState(audioMixer: mixer, defaults: try makeIsolatedDefaults())

        state.cleanupForTermination()
        state.cleanupForTermination()

        XCTAssertEqual(mixer.destroySharedMemoryCallCount, 1,
                       "cleanupForTermination must destroy exactly once")
    }
}
