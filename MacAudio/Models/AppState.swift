import SwiftUI
import CoreAudio
import Combine
import os

final class AppState: ObservableObject {
    @Published var isActive = false
    @Published var micVolume: Float = 0.7 {
        didSet { audioMixer?.micGain = micVolume }
    }
    @Published var systemVolume: Float = 0.7 {
        didSet { audioMixer?.systemGain = systemVolume }
    }
    @Published var selectedMicDeviceID: AudioDeviceID = kAudioObjectUnknown
    @Published var availableMicDevices: [AudioDevice] = []
    @Published var driverInstalled = false
    @Published var lastError: String?
    @Published var isReady = false

    private var audioMixer: AudioMixer?
    private var deviceChangeListener: AudioObjectPropertyListenerBlock?
    private let logger = Logger(subsystem: "com.macaudio.app", category: "state")

    init() {
        // Do CoreAudio queries on background thread, update UI on main
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            let devices = AudioDeviceManager.getInputDevices()
            let defaultDevice = AudioDeviceManager.getDefaultInputDevice()
            let installed = DriverInstaller.isDriverInstalled()

            DispatchQueue.main.async {
                guard let self = self else { return }
                self.availableMicDevices = devices
                self.selectedMicDeviceID = defaultDevice
                self.driverInstalled = installed
                self.isReady = true

                self.deviceChangeListener = AudioDeviceManager.listenForDeviceChanges { [weak self] in
                    self?.refreshDevicesAsync()
                }
                self.logger.info("AppState setup complete, \(devices.count) mic devices found, driver=\(installed)")
            }
        }
    }

    func toggleActive() {
        NSLog("toggleActive called, isActive=\(isActive)")
        if isActive {
            stopAudio()
        } else {
            startAudio()
        }
    }

    func updateMicDevice(_ deviceID: AudioDeviceID) {
        selectedMicDeviceID = deviceID
        audioMixer?.setMicDevice(deviceID)
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
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            let devices = AudioDeviceManager.getInputDevices()
            let defaultDevice = AudioDeviceManager.getDefaultInputDevice()

            DispatchQueue.main.async {
                guard let self = self else { return }
                self.availableMicDevices = devices
                if self.selectedMicDeviceID == kAudioObjectUnknown ||
                   !devices.contains(where: { $0.id == self.selectedMicDeviceID }) {
                    self.selectedMicDeviceID = defaultDevice
                }
            }
        }
    }

    func checkDriverInstalled() {
        driverInstalled = DriverInstaller.isDriverInstalled()
    }

    private func startAudio() {
        logger.info("startAudio called, driverInstalled=\(self.driverInstalled)")
        NSLog("startAudio called, driverInstalled=\(driverInstalled)")

        guard driverInstalled else {
            lastError = "Please install the audio driver first"
            isActive = false
            NSLog("ERROR: driver not installed")
            return
        }

        lastError = nil
        let mixer = AudioMixer()
        mixer.micGain = micVolume
        mixer.systemGain = systemVolume

        do {
            let deviceID = selectedMicDeviceID != kAudioObjectUnknown
                ? selectedMicDeviceID : nil
            NSLog("Starting mixer with device: \(String(describing: deviceID))")
            try mixer.start(micDeviceID: deviceID)
            audioMixer = mixer
            isActive = true
            // Show warning if system audio capture failed
            if let sysErr = mixer.systemCaptureError {
                lastError = sysErr
            }
            NSLog("Audio started successfully, isActive=\(isActive)")
            logger.info("Audio started")
        } catch {
            lastError = error.localizedDescription
            isActive = false
            NSLog("ERROR starting audio: \(error)")
            logger.error("Failed to start audio: \(error.localizedDescription)")
        }
    }

    private func stopAudio() {
        audioMixer?.stop()
        audioMixer = nil
        isActive = false
        lastError = nil
        logger.info("Audio stopped")
    }
}
