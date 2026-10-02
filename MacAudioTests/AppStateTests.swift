import XCTest
import CoreAudio
@testable import MacAudio

/// Mock behind the AudioMixering seam: records setMicDevice traffic and
/// returns a canned result so AppState's revert path can be driven
/// deterministically.
private final class MockAudioMixer: AudioMixering {
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
    private var savedDefaults: [String: Any?] = [:]

    private let oldDevice = AudioDevice(id: 100, name: "Old Mic", uid: "old-mic-uid")
    private let newDevice = AudioDevice(id: 200, name: "New Mic", uid: "new-mic-uid")

    override func setUp() {
        super.setUp()
        // AppState persists via UserDefaults.standard; snapshot the keys
        // updateMicDevice/savePreferences write so the test host's real
        // preferences survive the run.
        for key in ["micVolume", "systemVolume", "selectedMicDeviceID", "selectedMicDeviceUID"] {
            savedDefaults[key] = UserDefaults.standard.object(forKey: key)
        }
    }

    override func tearDown() {
        for (key, value) in savedDefaults {
            if let value {
                UserDefaults.standard.set(value, forKey: key)
            } else {
                UserDefaults.standard.removeObject(forKey: key)
            }
        }
        savedDefaults = [:]
        super.tearDown()
    }

    @MainActor
    func test_updateMicDevice_reverts_selection_and_uid_and_sets_error_when_switch_fails() {
        let mixer = MockAudioMixer()
        mixer.stubbedSwitchError = "switch failed"
        let state = AppState(audioMixer: mixer)
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
    }

    @MainActor
    func test_updateMicDevice_keeps_new_selection_and_leaves_last_error_untouched_on_success() {
        let mixer = MockAudioMixer()
        let state = AppState(audioMixer: mixer)
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
