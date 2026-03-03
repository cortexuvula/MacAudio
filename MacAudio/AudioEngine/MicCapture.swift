import CoreAudio
import AudioToolbox
import Foundation
import os

final class MicCapture {
    private var deviceID: AudioDeviceID = kAudioObjectUnknown
    private var ioProcID: AudioDeviceIOProcID?
    private var isRunning = false
    private let logger = Logger(subsystem: "com.macaudio.app", category: "mic")

    /// Callback delivers interleaved Float32 frames at the mic's native sample rate
    var onAudioReceived: ((UnsafePointer<Float>, UInt32, UInt32) -> Void)?

    func setInputDevice(_ id: AudioDeviceID) {
        deviceID = id
    }

    func start() throws {
        guard !isRunning else { return }

        // Use provided device or fall back to default input
        if deviceID == kAudioObjectUnknown {
            deviceID = Self.getDefaultInputDevice()
        }
        guard deviceID != kAudioObjectUnknown else {
            throw NSError(domain: "MacAudio", code: 1,
                          userInfo: [NSLocalizedDescriptionKey: "No input device available"])
        }

        // Query the device's native sample rate
        let sampleRate = Self.getDeviceSampleRate(deviceID)
        logger.info("Mic device ID=\(self.deviceID) sampleRate=\(sampleRate)")

        // Create IOProc
        var procID: AudioDeviceIOProcID?
        let status = AudioDeviceCreateIOProcIDWithBlock(&procID, deviceID, nil) {
            [weak self] _, inInputData, _, _, _ in
            guard let self = self else { return }
            self.handleIOCallback(inInputData)
        }
        guard status == noErr, let validProcID = procID else {
            logger.error("Failed to create mic IOProc: \(status)")
            throw NSError(domain: "MacAudio", code: Int(status),
                          userInfo: [NSLocalizedDescriptionKey: "Failed to create mic IOProc (status: \(status))"])
        }
        ioProcID = validProcID

        // Start IO
        let startStatus = AudioDeviceStart(deviceID, ioProcID)
        guard startStatus == noErr else {
            cleanup()
            logger.error("Failed to start mic IO: \(startStatus)")
            throw NSError(domain: "MacAudio", code: Int(startStatus),
                          userInfo: [NSLocalizedDescriptionKey: "Failed to start mic IO (status: \(startStatus))"])
        }

        isRunning = true
        logger.info("Mic capture started via IOProc")
    }

    private func handleIOCallback(_ inputData: UnsafePointer<AudioBufferList>) {
        let bufferList = inputData.pointee
        guard bufferList.mNumberBuffers >= 1 else { return }
        let firstBuffer = bufferList.mBuffers
        guard let data = firstBuffer.mData else { return }

        let bytesPerSample = UInt32(MemoryLayout<Float>.size)
        let channelCount = firstBuffer.mNumberChannels
        guard channelCount > 0, bytesPerSample > 0 else { return }
        let frameCount = firstBuffer.mDataByteSize / (bytesPerSample * channelCount)
        guard frameCount > 0 else { return }

        let floatPtr = data.assumingMemoryBound(to: Float.self)
        onAudioReceived?(floatPtr, frameCount, channelCount)
    }

    func stop() {
        guard isRunning else { return }
        isRunning = false
        cleanup()
        logger.info("Mic capture stopped")
    }

    private func cleanup() {
        if let proc = ioProcID, deviceID != kAudioObjectUnknown {
            AudioDeviceStop(deviceID, proc)
            AudioDeviceDestroyIOProcID(deviceID, proc)
            ioProcID = nil
        }
    }

    // MARK: - Helpers

    private static func getDefaultInputDevice() -> AudioDeviceID {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDefaultInputDevice,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var deviceID: AudioDeviceID = kAudioObjectUnknown
        var size = UInt32(MemoryLayout<AudioDeviceID>.size)
        AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size, &deviceID)
        return deviceID
    }

    private static func getDeviceSampleRate(_ deviceID: AudioDeviceID) -> Float64 {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyNominalSampleRate,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var sampleRate: Float64 = 0
        var size = UInt32(MemoryLayout<Float64>.size)
        AudioObjectGetPropertyData(deviceID, &address, 0, nil, &size, &sampleRate)
        return sampleRate
    }

    deinit {
        stop()
    }
}
