import SwiftUI
import CoreAudio
import Combine
import os

@MainActor
final class AppState: ObservableObject {
    @Published var isActive = false
    @Published var micVolume: Float = AudioConstants.defaultGain {
        didSet {
            audioMixer?.micGain = micVolume
            savePreferences()
        }
    }
    @Published var systemVolume: Float = AudioConstants.defaultGain {
        didSet {
            audioMixer?.systemGain = systemVolume
            savePreferences()
        }
    }
    @Published var selectedMicDeviceID: AudioDeviceID = kAudioObjectUnknown
    @Published var availableMicDevices: [AudioDevice] = []
    @Published var driverInstalled = false
    @Published var lastError: String?
    private var audioMixer: AudioMixer?
    private var deviceChangeListener: AudioObjectPropertyListenerBlock?
    private let logger = Logger(subsystem: "com.macaudio.app", category: "state")

    private static let micVolumeKey = "micVolume"
    private static let systemVolumeKey = "systemVolume"
    private static let selectedMicDeviceIDKey = "selectedMicDeviceID"

    init() {
        loadPreferences()

        Task.detached {
            let devices = AudioDeviceManager.getInputDevices()
            let defaultDevice = AudioDeviceManager.getDefaultInputDevice()
            let installed = DriverInstaller.isDriverInstalled()

            await MainActor.run { [weak self] in
                guard let self else { return }
                self.availableMicDevices = devices
                self.selectedMicDeviceID = defaultDevice
                self.driverInstalled = installed
                self.deviceChangeListener = AudioDeviceManager.listenForDeviceChanges { [weak self] in
                    self?.refreshDevicesAsync()
                }
                self.logger.info("AppState setup complete, \(devices.count) mic devices found, driver=\(installed)")
            }
        }
    }

    nonisolated deinit {
        if let listener = deviceChangeListener {
            var address = AudioObjectPropertyAddress(
                mSelector: kAudioHardwarePropertyDevices,
                mScope: kAudioObjectPropertyScopeGlobal,
                mElement: kAudioObjectPropertyElementMain
            )
            AudioObjectRemovePropertyListenerBlock(
                AudioObjectID(kAudioObjectSystemObject),
                &address,
                DispatchQueue.main,
                listener
            )
        }
    }

    func toggleActive() {
        logger.debug("toggleActive called, isActive=\(self.isActive)")
        if isActive {
            stopAudio()
        } else {
            startAudio()
        }
    }

    func updateMicDevice(_ deviceID: AudioDeviceID) {
        selectedMicDeviceID = deviceID
        audioMixer?.setMicDevice(deviceID)
        savePreferences()
    }

    func installDriver() {
        DriverInstaller.installDriver { [weak self] success in
            DispatchQueue.main.async {
                self?.driverInstalled = success
                if !success {
                    self?.lastError = "Driver installation failed"
                }
            }
        }
    }

    func refreshDevicesAsync() {
        Task.detached {
            let devices = AudioDeviceManager.getInputDevices()
            let defaultDevice = AudioDeviceManager.getDefaultInputDevice()

            await MainActor.run { [weak self] in
                guard let self else { return }
                self.availableMicDevices = devices
                if self.selectedMicDeviceID == kAudioObjectUnknown ||
                   !devices.contains(where: { $0.id == self.selectedMicDeviceID }) {
                    self.selectedMicDeviceID = defaultDevice
                }
            }
        }
    }

    private func startAudio() {
        logger.info("startAudio called, driverInstalled=\(self.driverInstalled)")

        guard driverInstalled else {
            lastError = "Please install the audio driver first"
            isActive = false
            logger.error("Driver not installed")
            return
        }

        lastError = nil
        let mixer = AudioMixer()
        mixer.micGain = micVolume
        mixer.systemGain = systemVolume

        do {
            let deviceID = selectedMicDeviceID != kAudioObjectUnknown
                ? selectedMicDeviceID : nil
            logger.info("Starting mixer with device: \(String(describing: deviceID))")
            try mixer.start(micDeviceID: deviceID)
            audioMixer = mixer
            isActive = true
            // Show warning if system audio capture failed
            if let sysErr = mixer.systemCaptureError {
                lastError = sysErr
            }
            logger.info("Audio started")
        } catch {
            lastError = error.localizedDescription
            isActive = false
            logger.error("Failed to start audio: \(error.localizedDescription)")
        }
    }

    private func stopAudio() {
        audioMixer?.stop()
        audioMixer?.destroySharedMemory()
        audioMixer = nil
        isActive = false
        lastError = nil
        logger.info("Audio stopped")
    }

    private func savePreferences() {
        UserDefaults.standard.set(micVolume, forKey: Self.micVolumeKey)
        UserDefaults.standard.set(systemVolume, forKey: Self.systemVolumeKey)
        UserDefaults.standard.set(selectedMicDeviceID, forKey: Self.selectedMicDeviceIDKey)
    }

    private func loadPreferences() {
        if UserDefaults.standard.object(forKey: Self.micVolumeKey) != nil {
            micVolume = UserDefaults.standard.float(forKey: Self.micVolumeKey)
        }
        if UserDefaults.standard.object(forKey: Self.systemVolumeKey) != nil {
            systemVolume = UserDefaults.standard.float(forKey: Self.systemVolumeKey)
        }
        let savedDeviceID = UserDefaults.standard.integer(forKey: Self.selectedMicDeviceIDKey)
        if savedDeviceID != 0 {
            selectedMicDeviceID = AudioDeviceID(savedDeviceID)
        }
    }
}
