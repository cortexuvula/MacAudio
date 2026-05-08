import CoreAudio
import Foundation

struct AudioDevice: Identifiable, Hashable {
    let id: AudioDeviceID
    let name: String
    let uid: String
}

final class AudioDeviceManager {
    static func getInputDevices() -> [AudioDevice] {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDevices,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )

        var dataSize: UInt32 = 0
        var status = AudioObjectGetPropertyDataSize(
            AudioObjectID(kAudioObjectSystemObject),
            &address, 0, nil, &dataSize
        )
        guard status == noErr else { return [] }

        let deviceCount = Int(dataSize) / MemoryLayout<AudioDeviceID>.size
        var deviceIDs = [AudioDeviceID](repeating: 0, count: deviceCount)
        status = AudioObjectGetPropertyData(
            AudioObjectID(kAudioObjectSystemObject),
            &address, 0, nil, &dataSize, &deviceIDs
        )
        guard status == noErr else { return [] }

        var inputDevices: [AudioDevice] = []

        for deviceID in deviceIDs {
            // Check if device has input streams
            var streamAddress = AudioObjectPropertyAddress(
                mSelector: kAudioDevicePropertyStreamConfiguration,
                mScope: kAudioObjectPropertyScopeInput,
                mElement: kAudioObjectPropertyElementMain
            )

            var streamDataSize: UInt32 = 0
            status = AudioObjectGetPropertyDataSize(
                deviceID, &streamAddress, 0, nil, &streamDataSize
            )
            guard status == noErr, streamDataSize > 0 else { continue }

            let bufferListData = UnsafeMutableRawPointer.allocate(
                byteCount: Int(streamDataSize),
                alignment: MemoryLayout<AudioBufferList>.alignment
            )
            defer { bufferListData.deallocate() }

            status = AudioObjectGetPropertyData(
                deviceID, &streamAddress, 0, nil, &streamDataSize, bufferListData
            )
            guard status == noErr else { continue }

            let bufferList = bufferListData.assumingMemoryBound(to: AudioBufferList.self)
            var hasInput = false
            let bufferCount = Int(bufferList.pointee.mNumberBuffers)
            if bufferCount > 0 {
                let buffers = UnsafeBufferPointer<AudioBuffer>(
                    start: &bufferList.pointee.mBuffers,
                    count: bufferCount
                )
                for buffer in buffers {
                    if buffer.mNumberChannels > 0 {
                        hasInput = true
                        break
                    }
                }
            }
            guard hasInput else { continue }

            // Skip our own virtual device
            let uid = getDeviceUID(deviceID)
            if uid == AudioConstants.virtualDeviceUID { continue }

            let name = getDeviceName(deviceID)
            inputDevices.append(AudioDevice(id: deviceID, name: name, uid: uid))
        }

        return inputDevices
    }

    static func getDefaultInputDevice() -> AudioDeviceID {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDefaultInputDevice,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var deviceID: AudioDeviceID = kAudioObjectUnknown
        var dataSize = UInt32(MemoryLayout<AudioDeviceID>.size)
        AudioObjectGetPropertyData(
            AudioObjectID(kAudioObjectSystemObject),
            &address, 0, nil, &dataSize, &deviceID
        )
        return deviceID
    }

    static func listenForDeviceChanges(callback: @escaping () -> Void) -> AudioObjectPropertyListenerBlock {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDevices,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )

        let block: AudioObjectPropertyListenerBlock = { _, _ in
            callback()
        }

        AudioObjectAddPropertyListenerBlock(
            AudioObjectID(kAudioObjectSystemObject),
            &address,
            DispatchQueue.main,
            block
        )

        return block
    }

    private static func getDeviceName(_ deviceID: AudioDeviceID) -> String {
        return readCFStringProperty(deviceID, selector: kAudioObjectPropertyName) ?? "Unknown Device"
    }

    private static func getDeviceUID(_ deviceID: AudioDeviceID) -> String {
        return readCFStringProperty(deviceID, selector: kAudioDevicePropertyDeviceUID) ?? ""
    }

    /// Reads a CFString property from a Core Audio object and consumes its +1 retain.
    /// Using `Unmanaged<CFString>?` (instead of a typed `CFString` local) keeps Swift's
    /// reference-counting out of the path; we explicitly consume the retain.
    private static func readCFStringProperty(_ objectID: AudioObjectID,
                                             selector: AudioObjectPropertySelector) -> String? {
        var address = AudioObjectPropertyAddress(
            mSelector: selector,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var unmanaged: Unmanaged<CFString>?
        var dataSize = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
        let status = AudioObjectGetPropertyData(
            objectID, &address, 0, nil, &dataSize, &unmanaged
        )
        guard status == noErr, let value = unmanaged else { return nil }
        return value.takeRetainedValue() as String
    }
}
