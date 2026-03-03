import SwiftUI
import CoreAudio
import Combine
import AVFoundation
import ServiceManagement
import os

enum CaptureStatus {
    case stopped, micOnly, both
}

@MainActor
final class AppState: ObservableObject {
    @Published var isActive = false
    @Published var captureStatus: CaptureStatus = .stopped
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
    @Published var micPermissionGranted = false
    @Published var screenCapturePermissionGranted = false
    @Published var isSetupComplete = false
    @Published var launchAtLogin = false
    @Published var autoStartCapture = false
    private var audioMixer: AudioMixer?
    private var deviceChangeListener: AudioObjectPropertyListenerBlock?
    private let logger = Logger(subsystem: "com.macaudio.app", category: "state")

    private static let micVolumeKey = "micVolume"
    private static let systemVolumeKey = "systemVolume"
    private static let selectedMicDeviceIDKey = "selectedMicDeviceID"
    private static let autoStartCaptureKey = "autoStartCapture"

    init() {
        loadPreferences()

        Task.detached {
            let devices = AudioDeviceManager.getInputDevices()
            let defaultDevice = AudioDeviceManager.getDefaultInputDevice()
            let installed = DriverInstaller.isDriverInstalled()
            let micAuth = AVCaptureDevice.authorizationStatus(for: .audio) == .authorized
            let screenAuth = CGPreflightScreenCaptureAccess()

            await MainActor.run { [weak self] in
                guard let self else { return }
                self.availableMicDevices = devices
                self.selectedMicDeviceID = defaultDevice
                self.driverInstalled = installed
                self.micPermissionGranted = micAuth
                self.screenCapturePermissionGranted = screenAuth
                self.deviceChangeListener = AudioDeviceManager.listenForDeviceChanges { [weak self] in
                    self?.refreshDevicesAsync()
                }
                self.logger.info("AppState setup complete, \(devices.count) mic devices found, driver=\(installed), mic=\(micAuth), screen=\(screenAuth)")
                self.launchAtLogin = SMAppService.mainApp.status == .enabled
                self.isSetupComplete = true
            }
        }
    }

    deinit {
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
        DriverInstaller.installDriver { [weak self] result in
            DispatchQueue.main.async {
                switch result {
                case .success:
                    self?.driverInstalled = true
                    self?.lastError = nil
                case .failure(let error):
                    self?.driverInstalled = DriverInstaller.isDriverInstalled()
                    self?.lastError = error.localizedDescription
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

    func toggleLaunchAtLogin() {
        let service = SMAppService.mainApp
        do {
            if service.status == .enabled {
                try service.unregister()
                launchAtLogin = false
                autoStartCapture = false
                savePreferences()
            } else {
                try service.register()
                launchAtLogin = true
            }
        } catch {
            launchAtLogin = service.status == .enabled
            lastError = "Failed to update login item: \(error.localizedDescription)"
            logger.error("SMAppService error: \(error.localizedDescription)")
        }
    }

    func toggleAutoStartCapture() {
        autoStartCapture.toggle()
        savePreferences()
    }

    func attemptAutoStart() {
        guard autoStartCapture else { return }
        guard driverInstalled else {
            lastError = "Auto-start failed: audio driver not installed"
            logger.warning("Auto-start skipped: driver not installed")
            return
        }
        guard micPermissionGranted else {
            lastError = "Auto-start failed: microphone permission not granted"
            logger.warning("Auto-start skipped: mic permission not granted")
            return
        }
        logger.info("Auto-starting audio capture")
        startAudio()
    }

    func requestMicPermission() {
        Task {
            let granted = await AVCaptureDevice.requestAccess(for: .audio)
            await MainActor.run {
                self.micPermissionGranted = granted
                if !granted {
                    self.lastError = "Microphone access denied — grant in System Settings > Privacy > Microphone"
                }
            }
        }
    }

    func requestScreenPermission() {
        CGRequestScreenCaptureAccess()
        // Re-check after a short delay (user must grant in System Settings)
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) { [weak self] in
            self?.screenCapturePermissionGranted = CGPreflightScreenCaptureAccess()
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

        if !micPermissionGranted {
            requestMicPermission()
        }

        if !screenCapturePermissionGranted {
            logger.warning("Screen capture permission not granted, system audio may not work")
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
            if let sysErr = mixer.systemCaptureError {
                lastError = sysErr
                captureStatus = .micOnly
            } else {
                captureStatus = .both
            }
            logger.info("Audio started, status=\(String(describing: self.captureStatus))")
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
        captureStatus = .stopped
        lastError = nil
        logger.info("Audio stopped")
    }

    private func savePreferences() {
        UserDefaults.standard.set(micVolume, forKey: Self.micVolumeKey)
        UserDefaults.standard.set(systemVolume, forKey: Self.systemVolumeKey)
        UserDefaults.standard.set(selectedMicDeviceID, forKey: Self.selectedMicDeviceIDKey)
        UserDefaults.standard.set(autoStartCapture, forKey: Self.autoStartCaptureKey)
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
        autoStartCapture = UserDefaults.standard.bool(forKey: Self.autoStartCaptureKey)
    }
}
