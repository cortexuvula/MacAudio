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
    private static let selectedMicDeviceUIDKey = "selectedMicDeviceUID"
    private static let autoStartCaptureKey = "autoStartCapture"

    /// Preferred mic identified by stable UID. AudioDeviceID is a per-boot
    /// numeric handle; UID survives unplug/replug and reboots, so we prefer it
    /// when matching saved selection against the live device list.
    private var preferredMicUID: String?

    /// Set when loadPreferences() read autoStartCapture from the plist file
    /// because cfprefsd hadn't loaded our domain yet. Prevents attemptAutoStart()
    /// from overwriting the known-good plist value with a stale cfprefsd cache
    /// hit (nil → non-nil-but-wrong between the two reads).
    private var didLoadAutoStartFromPlist = false

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
                self.selectedMicDeviceID = self.resolveMicSelection(in: devices, fallback: defaultDevice)
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
        // Remember by UID so a brief unplug doesn't lose the user's preference.
        preferredMicUID = availableMicDevices.first(where: { $0.id == deviceID })?.uid
        audioMixer?.setMicDevice(deviceID)
        savePreferences()
    }

    /// Picks the best `AudioDeviceID` for the user's saved mic preference,
    /// preferring UID match over numeric ID. Returns `fallback` if nothing matches.
    private func resolveMicSelection(in devices: [AudioDevice],
                                     fallback: AudioDeviceID) -> AudioDeviceID {
        if let uid = preferredMicUID,
           let match = devices.first(where: { $0.uid == uid }) {
            return match.id
        }
        if selectedMicDeviceID != kAudioObjectUnknown,
           let match = devices.first(where: { $0.id == selectedMicDeviceID }) {
            // Cache the UID so future refreshes survive an unplug.
            preferredMicUID = match.uid
            return match.id
        }
        return fallback
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
                self.selectedMicDeviceID = self.resolveMicSelection(in: devices, fallback: defaultDevice)
            }
        }
    }

    func toggleLaunchAtLogin() {
        let service = SMAppService.mainApp
        let wasEnabled = service.status == .enabled
        do {
            if wasEnabled {
                try service.unregister()
                launchAtLogin = false
                autoStartCapture = false
                saveAutoStartPreference()
                logger.notice("Launch at Login: unregistered (auto-start also cleared)")
            } else {
                try service.register()
                launchAtLogin = true
                logger.notice("Launch at Login: registered")
            }
        } catch {
            launchAtLogin = service.status == .enabled
            lastError = "Failed to update login item: \(error.localizedDescription)"
            logger.error("SMAppService error (wasEnabled=\(wasEnabled)): \(error.localizedDescription)")
        }
    }

    func toggleAutoStartCapture() {
        let willEnable = !autoStartCapture
        logger.notice("Auto-Start Capturing: toggle clicked (willEnable=\(willEnable), launchAtLogin=\(self.launchAtLogin))")
        if willEnable && !launchAtLogin {
            // Auto-start requires the app to launch at login. Enable that first.
            toggleLaunchAtLogin()
            guard launchAtLogin else {
                if lastError == nil {
                    lastError = "Approve MacAudio in System Settings → General → Login Items, then try again."
                }
                logger.notice("Auto-Start Capturing: aborted, Launch at Login could not be enabled")
                return
            }
        }
        autoStartCapture.toggle()
        saveAutoStartPreference()
        logger.notice("Auto-Start Capturing: now \(self.autoStartCapture ? "ON" : "OFF") (saved)")
    }

    private static let autoStartMaxRetries = 10
    private static let autoStartRetryDelay: TimeInterval = 1.0
    private static let autoStartPrefRetryDelay: TimeInterval = 2.0

    /// Reads the `autoStartCapture` value directly from the on-disk plist,
    /// bypassing the cfprefsd XPC channel. This is the last-resort fallback
    /// when the app launches at login and cfprefsd hasn't loaded the app's
    /// preference domain into its cache yet — in that state,
    /// `UserDefaults.standard.object(forKey:)` returns `nil` even though the
    /// plist file on disk has the correct value.
    private static func readAutoStartFromPlistFile() -> Bool? {
        let home = FileManager.default.homeDirectoryForCurrentUser.path
        let plistPath = "\(home)/Library/Preferences/com.macaudio.app.plist"
        guard let dict = NSDictionary(contentsOfFile: plistPath) else { return nil }
        return dict[autoStartCaptureKey] as? Bool
    }

    func attemptAutoStart(retriesRemaining: Int = AppState.autoStartMaxRetries) {
        // cfprefsd may have been slow at init, leaving the in-memory pref stale
        // (defaulted to false even though the user had it on). By the time
        // auto-start fires, cfprefsd is reliably available — re-read and resync.
        //
        // HOWEVER: if loadPreferences() fell back to the plist file (because
        // cfprefsd returned nil), skip the refresh on the FIRST attempt.
        // cfprefsd may now return a non-nil but STALE cached value (e.g. false
        // from a previous session) — overwriting the correct plist value.
        // On retries (cfprefsd had more time to sync), the refresh is safe.
        if !didLoadAutoStartFromPlist || retriesRemaining < Self.autoStartMaxRetries {
            if UserDefaults.standard.object(forKey: Self.autoStartCaptureKey) != nil {
                let liveValue = UserDefaults.standard.bool(forKey: Self.autoStartCaptureKey)
                if autoStartCapture != liveValue {
                    logger.notice("Auto-start: pref refreshed from disk (in-memory=\(self.autoStartCapture) → live=\(liveValue))")
                    autoStartCapture = liveValue
                }
            }
        } else {
            logger.notice("Auto-start: skipping UserDefaults refresh (loaded from plist, first attempt)")
        }

        guard autoStartCapture else {
            // cfprefsd may still not have loaded our domain — object(forKey:)
            // returned nil above and boolIfPresent defaulted to false. As a
            // last resort, read the plist file directly (bypasses cfprefsd).
            if let plistValue = Self.readAutoStartFromPlistFile(), plistValue {
                logger.notice("Auto-start: cfprefsd missed pref, plist says ON — retrying in \(Self.autoStartPrefRetryDelay)s")
                autoStartCapture = true
                DispatchQueue.main.asyncAfter(deadline: .now() + Self.autoStartPrefRetryDelay) { [weak self] in
                    self?.attemptAutoStart(retriesRemaining: retriesRemaining - 1)
                }
                return
            }
            logger.notice("Auto-start: skipped (autoStartCapture is off)")
            return
        }

        let driverNow = DriverInstaller.isDriverInstalled()
        let micNow = AVCaptureDevice.authorizationStatus(for: .audio) == .authorized

        // Sync UI state with live values so the menu reflects reality.
        if driverInstalled != driverNow { driverInstalled = driverNow }
        if micPermissionGranted != micNow { micPermissionGranted = micNow }

        logger.notice("Auto-start: check driver=\(driverNow) mic=\(micNow) retriesLeft=\(retriesRemaining)")

        if !driverNow || !micNow {
            if retriesRemaining > 0 {
                DispatchQueue.main.asyncAfter(deadline: .now() + Self.autoStartRetryDelay) { [weak self] in
                    self?.attemptAutoStart(retriesRemaining: retriesRemaining - 1)
                }
                return
            }
            if !driverNow {
                lastError = "Auto-start failed: audio driver not installed"
                logger.error("Auto-start aborted: driver not installed after retries")
            } else {
                lastError = "Auto-start failed: microphone permission not granted"
                logger.error("Auto-start aborted: mic permission not granted after retries")
            }
            return
        }

        logger.notice("Auto-start: starting audio capture")
        startAudio()
    }

    func requestMicPermission(thenStart: Bool = false) {
        Task {
            let granted = await AVCaptureDevice.requestAccess(for: .audio)
            await MainActor.run {
                self.micPermissionGranted = granted
                if !granted {
                    self.lastError = "Microphone access denied — grant in System Settings > Privacy > Microphone"
                } else if thenStart {
                    self.startAudio()
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

    /// Re-reads current TCC state for mic and screen capture. Call when the app
    /// becomes active so the menu reflects changes the user just made in
    /// System Settings without restarting MacAudio.
    func refreshPermissionState() {
        let micAuth = AVCaptureDevice.authorizationStatus(for: .audio) == .authorized
        let screenAuth = CGPreflightScreenCaptureAccess()
        if micPermissionGranted != micAuth { micPermissionGranted = micAuth }
        if screenCapturePermissionGranted != screenAuth { screenCapturePermissionGranted = screenAuth }
    }

    private func startAudio() {
        logger.info("startAudio called, driverInstalled=\(self.driverInstalled), isActive=\(self.isActive)")

        guard !isActive else {
            logger.info("startAudio: already active, ignoring")
            return
        }

        guard driverInstalled else {
            lastError = "Please install the audio driver first"
            isActive = false
            logger.error("Driver not installed")
            return
        }

        if !micPermissionGranted {
            // Defer start until permission is granted; the request callback re-enters startAudio.
            requestMicPermission(thenStart: true)
            return
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

    func cleanupForTermination() {
        audioMixer?.stop()
        audioMixer?.destroySharedMemory()
        audioMixer = nil
    }

    private func stopAudio() {
        audioMixer?.stop()
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
        if let uid = preferredMicUID {
            UserDefaults.standard.set(uid, forKey: Self.selectedMicDeviceUIDKey)
        }
        // NOTE: autoStartCapture is intentionally NOT saved here.
        // savePreferences() is called from didSet observers (volume changes)
        // and cleanupForTermination(). Writing autoStartCapture from those
        // paths can overwrite the correct on-disk value with a stale in-memory
        // value — specifically when cfprefsd returned a wrong value at login
        // and auto-start never ran. See saveAutoStartPreference() below.
    }

    /// Writes `autoStartCapture` to UserDefaults and flushes to disk.
    /// Only called when the user explicitly toggles the setting, so the
    /// in-memory value is guaranteed to reflect the user's intent (not a
    /// stale cfprefsd cache hit from login).
    private func saveAutoStartPreference() {
        UserDefaults.standard.set(autoStartCapture, forKey: Self.autoStartCaptureKey)
        UserDefaults.standard.synchronize()
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
        preferredMicUID = UserDefaults.standard.string(forKey: Self.selectedMicDeviceUIDKey)
        autoStartCapture = UserDefaults.standard.boolIfPresent(
            forKey: Self.autoStartCaptureKey, default: autoStartCapture)

        // The plist file is the most trustworthy source — it's written by
        // synchronize() (direct disk write) and read by NSDictionary(contentsOfFile:)
        // (direct disk read), bypassing cfprefsd entirely. If cfprefsd returns
        // a value that disagrees with the plist, the plist is almost certainly
        // correct and cfprefsd has a stale cache entry (common at login).
        if let plistValue = Self.readAutoStartFromPlistFile() {
            if autoStartCapture != plistValue {
                logger.notice("loadPreferences: cfprefsd says \(self.autoStartCapture) but plist says \(plistValue) — trusting plist")
                autoStartCapture = plistValue
                didLoadAutoStartFromPlist = true
            }
        } else if UserDefaults.standard.object(forKey: Self.autoStartCaptureKey) == nil {
            // cfprefsd returned nil AND no plist file — keep the default (false)
        }
    }
}
