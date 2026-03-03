import SwiftUI
import AppKit
import Combine
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

class AppDelegate: NSObject, NSApplicationDelegate {
    private var statusItem: NSStatusItem?
    private let appState = AppState()
    private let logger = Logger(subsystem: "com.macaudio.app", category: "delegate")

    func applicationDidFinishLaunching(_ notification: Notification) {
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)

        if let button = statusItem?.button {
            button.image = NSImage(systemSymbolName: "speaker.fill", accessibilityDescription: "MacAudio")
        }

        buildMenu()

        // Observe state changes to rebuild menu
        appState.objectWillChange
            .receive(on: RunLoop.main)
            .sink { [weak self] _ in
                self?.buildMenu()
            }
            .store(in: &cancellables)
    }

    private var cancellables = Set<AnyCancellable>()

    private func buildMenu() {
        guard let statusItem else { return }
        let menu = NSMenu()

        let startStop = NSMenuItem(
            title: appState.isActive ? "Stop" : "Start",
            action: #selector(toggleActive),
            keyEquivalent: ""
        )
        startStop.target = self
        menu.addItem(startStop)

        menu.addItem(NSMenuItem.separator())

        // Microphone submenu
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
}
