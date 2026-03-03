import SwiftUI
import AppKit
import Combine
import AVFoundation
import os

@main
struct MacAudioApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var appDelegate

    var body: some Scene {
        Settings {
            EmptyView()
        }
    }
}

@MainActor
class AppDelegate: NSObject, NSApplicationDelegate {
    private var statusItem: NSStatusItem?
    private let appState = AppState()
    private let logger = Logger(subsystem: "com.macaudio.app", category: "delegate")
    private var cancellables = Set<AnyCancellable>()

    func applicationDidFinishLaunching(_ notification: Notification) {
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)

        if let button = statusItem?.button {
            button.image = NSImage(systemSymbolName: "waveform.circle", accessibilityDescription: "MacAudio")
        }

        buildMenu()

        // Observe only properties that require a full menu rebuild.
        // Volume changes flow through didSet to AudioMixer + UserDefaults
        // without triggering a menu rebuild (avoids losing slider focus).
        Publishers.MergeMany(
            appState.$isActive.map { _ in () }.eraseToAnyPublisher(),
            appState.$availableMicDevices.map { _ in () }.eraseToAnyPublisher(),
            appState.$selectedMicDeviceID.map { _ in () }.eraseToAnyPublisher(),
            appState.$driverInstalled.map { _ in () }.eraseToAnyPublisher(),
            appState.$lastError.map { _ in () }.eraseToAnyPublisher(),
            appState.$micPermissionGranted.map { _ in () }.eraseToAnyPublisher(),
            appState.$screenCapturePermissionGranted.map { _ in () }.eraseToAnyPublisher(),
            appState.$captureStatus.map { _ in () }.eraseToAnyPublisher()
        )
        .receive(on: RunLoop.main)
        .sink { [weak self] _ in
            self?.buildMenu()
            self?.updateStatusIcon()
        }
        .store(in: &cancellables)
    }

    private func updateStatusIcon() {
        guard let button = statusItem?.button else { return }
        let symbolName: String
        switch appState.captureStatus {
        case .stopped:
            symbolName = "waveform.circle"
        case .both:
            symbolName = "waveform.circle.fill"
        case .micOnly:
            symbolName = "mic.circle.fill"
        }
        button.image = NSImage(systemSymbolName: symbolName, accessibilityDescription: "MacAudio")
    }

    private func buildMenu() {
        guard let statusItem else { return }
        let menu = NSMenu()

        // Start/Stop
        let startStop = NSMenuItem(
            title: appState.isActive ? "Stop" : "Start",
            action: #selector(toggleActive),
            keyEquivalent: ""
        )
        startStop.target = self
        menu.addItem(startStop)

        // Capture status (when active)
        if appState.isActive {
            menu.addItem(NSMenuItem.separator())

            let micStatus = NSMenuItem(title: "Mic: Capturing", action: nil, keyEquivalent: "")
            micStatus.isEnabled = false
            menu.addItem(micStatus)

            if appState.captureStatus == .both {
                let sysStatus = NSMenuItem(title: "System Audio: Capturing", action: nil, keyEquivalent: "")
                sysStatus.isEnabled = false
                menu.addItem(sysStatus)
            } else {
                let sysStatus = NSMenuItem(
                    title: "System Audio: Permission needed",
                    action: #selector(requestScreenPermission),
                    keyEquivalent: ""
                )
                sysStatus.target = self
                menu.addItem(sysStatus)
            }
        }

        // Permission warnings (only when not all granted)
        if !appState.micPermissionGranted || !appState.screenCapturePermissionGranted {
            menu.addItem(NSMenuItem.separator())

            if !appState.micPermissionGranted {
                let micPerm = NSMenuItem(
                    title: "\u{26A0} Microphone: Grant Access...",
                    action: #selector(requestMicPermission),
                    keyEquivalent: ""
                )
                micPerm.target = self
                menu.addItem(micPerm)
            }

            if !appState.screenCapturePermissionGranted {
                let screenPerm = NSMenuItem(
                    title: "\u{26A0} Screen Recording: Open Settings...",
                    action: #selector(requestScreenPermission),
                    keyEquivalent: ""
                )
                screenPerm.target = self
                menu.addItem(screenPerm)
            }
        }

        // Volume sliders
        menu.addItem(NSMenuItem.separator())
        menu.addItem(makeSliderMenuItem(label: "Mic Volume", value: appState.micVolume, action: #selector(micSliderChanged(_:))))
        menu.addItem(makeSliderMenuItem(label: "System Volume", value: appState.systemVolume, action: #selector(systemSliderChanged(_:))))

        // Microphone submenu
        menu.addItem(NSMenuItem.separator())
        let micMenu = NSMenu()
        for device in appState.availableMicDevices {
            let item = NSMenuItem(
                title: device.name,
                action: #selector(selectMicDevice(_:)),
                keyEquivalent: ""
            )
            item.target = self
            item.representedObject = device.id
            item.state = (device.id == appState.selectedMicDeviceID) ? .on : .off
            micMenu.addItem(item)
        }
        let micItem = NSMenuItem(title: "Microphone", action: nil, keyEquivalent: "")
        micItem.submenu = micMenu
        menu.addItem(micItem)

        menu.addItem(NSMenuItem.separator())

        // Error display
        if let error = appState.lastError {
            let errorItem = NSMenuItem(title: error, action: nil, keyEquivalent: "")
            errorItem.isEnabled = false
            menu.addItem(errorItem)
        }

        // Driver status
        if appState.driverInstalled {
            let driverItem = NSMenuItem(title: "Driver: Installed", action: nil, keyEquivalent: "")
            driverItem.isEnabled = false
            menu.addItem(driverItem)
        } else {
            let installItem = NSMenuItem(
                title: "Install Audio Driver...",
                action: #selector(installDriver),
                keyEquivalent: ""
            )
            installItem.target = self
            menu.addItem(installItem)
        }

        menu.addItem(NSMenuItem.separator())

        let quitItem = NSMenuItem(
            title: "Quit MacAudio",
            action: #selector(NSApplication.terminate(_:)),
            keyEquivalent: "q"
        )
        menu.addItem(quitItem)

        statusItem.menu = menu
    }

    // MARK: - Volume Slider

    private func makeSliderMenuItem(label: String, value: Float, action: Selector) -> NSMenuItem {
        let containerWidth: CGFloat = 250
        let container = NSView(frame: NSRect(x: 0, y: 0, width: containerWidth, height: 30))

        let textField = NSTextField(labelWithString: label)
        textField.font = NSFont.menuFont(ofSize: 12)
        textField.frame = NSRect(x: 14, y: 5, width: 90, height: 20)
        container.addSubview(textField)

        let slider = NSSlider(value: Double(value), minValue: 0.0, maxValue: 1.0, target: self, action: action)
        slider.frame = NSRect(x: 108, y: 5, width: containerWidth - 122, height: 20)
        slider.isContinuous = true
        container.addSubview(slider)

        let menuItem = NSMenuItem()
        menuItem.view = container
        return menuItem
    }

    // MARK: - Actions

    @objc private func toggleActive() {
        logger.debug("toggleActive called, isActive=\(self.appState.isActive)")
        appState.toggleActive()
        logger.debug("after toggle, isActive=\(self.appState.isActive), error=\(self.appState.lastError ?? "none")")
    }

    @objc private func selectMicDevice(_ sender: NSMenuItem) {
        if let deviceID = sender.representedObject as? UInt32 {
            appState.updateMicDevice(deviceID)
        }
    }

    @objc private func installDriver() {
        appState.installDriver()
    }

    @objc private func requestMicPermission() {
        appState.requestMicPermission()
    }

    @objc private func requestScreenPermission() {
        appState.requestScreenPermission()
    }

    @objc private func micSliderChanged(_ sender: NSSlider) {
        appState.micVolume = Float(sender.doubleValue)
    }

    @objc private func systemSliderChanged(_ sender: NSSlider) {
        appState.systemVolume = Float(sender.doubleValue)
    }
}
