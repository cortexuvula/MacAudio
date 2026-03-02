import SwiftUI

struct MenuBarView: View {
    @EnvironmentObject var appState: AppState

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Text("MacAudio")
                    .font(.headline)
                Spacer()
                Circle()
                    .fill(appState.isActive ? Color.green : Color.gray)
                    .frame(width: 8, height: 8)
                Text(appState.isActive ? "Active" : "Inactive")
                    .font(.caption)
                    .foregroundColor(.secondary)
            }

            Divider()

            Toggle("Active", isOn: Binding(
                get: { appState.isActive },
                set: { _ in appState.toggleActive() }
            ))

            if appState.isActive, let error = appState.lastError {
                Text(error)
                    .font(.caption)
                    .foregroundColor(.red)
                    .lineLimit(2)
            }

            Divider()

            DevicePickerView(
                selectedDevice: Binding(
                    get: { appState.selectedMicDeviceID },
                    set: { appState.updateMicDevice($0) }
                ),
                devices: appState.availableMicDevices
            )

            VolumeSlider(label: "Mic Volume",
                         value: $appState.micVolume,
                         icon: "mic.fill")

            VolumeSlider(label: "System Audio",
                         value: $appState.systemVolume,
                         icon: "speaker.wave.2.fill")

            Divider()

            if !appState.driverInstalled {
                Button("Install Audio Driver...") {
                    appState.installDriver()
                }
                .buttonStyle(.borderedProminent)
            } else {
                HStack {
                    Image(systemName: "checkmark.circle.fill")
                        .foregroundColor(.green)
                        .font(.caption)
                    Text("Driver installed")
                        .font(.caption)
                        .foregroundColor(.secondary)
                }
            }

            Button("Quit MacAudio") {
                NSApplication.shared.terminate(nil)
            }
        }
        .padding()
        .frame(width: 280)
    }
}
