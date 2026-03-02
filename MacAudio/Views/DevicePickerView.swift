import SwiftUI
import CoreAudio

struct DevicePickerView: View {
    @Binding var selectedDevice: AudioDeviceID
    let devices: [AudioDevice]

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text("Microphone")
                .font(.caption)
                .foregroundColor(.secondary)
            Picker("Mic Source", selection: $selectedDevice) {
                ForEach(devices) { device in
                    Text(device.name).tag(device.id)
                }
            }
            .labelsHidden()
        }
    }
}
