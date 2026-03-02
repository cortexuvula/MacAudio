import SwiftUI

struct VolumeSlider: View {
    let label: String
    @Binding var value: Float
    let icon: String

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(label)
                .font(.caption)
                .foregroundColor(.secondary)
            HStack {
                Image(systemName: icon)
                    .frame(width: 16)
                    .foregroundColor(.secondary)
                Slider(value: Binding(
                    get: { Double(value) },
                    set: { value = Float($0) }
                ), in: 0...1)
                Text("\(Int(value * 100))%")
                    .font(.caption)
                    .monospacedDigit()
                    .frame(width: 36, alignment: .trailing)
            }
        }
    }
}
