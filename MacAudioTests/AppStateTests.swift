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

    func start(micDeviceID: AudioDeviceID?) throws {}

    func stop() {
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
/// selection sticks and lastError is untouched.
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

        state.updateMicDevice(newDevice.id)

        XCTAssertEqual(mixer.setMicDeviceCalls, [newDevice.id],
                       "the mixer must be asked to switch to the new device")
        XCTAssertEqual(state.selectedMicDeviceID, oldDevice.id,
                       "selection must revert to the device that is still capturing")
        XCTAssertEqual(state.preferredMicUID, oldDevice.uid,
                       "UID preference must revert to the device that is still capturing")
        XCTAssertEqual(state.lastError, "switch failed",
                       "the switch failure must surface to the user")
        XCTAssertEqual(defaults.integer(forKey: "selectedMicDeviceID"), Int(oldDevice.id),
                       "savePreferences must persist the reverted device selection")
        XCTAssertEqual(defaults.string(forKey: "selectedMicDeviceUID"), oldDevice.uid,
                       "savePreferences must persist the reverted device UID preference")
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
