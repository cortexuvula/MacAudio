import CoreAudio
import AudioToolbox
import Foundation
import os

final class SystemAudioCapture {
    private var tapObjectID: AudioObjectID = kAudioObjectUnknown
    private var aggregateDeviceID: AudioDeviceID = kAudioObjectUnknown
    private var ioProcID: AudioDeviceIOProcID?
    private var isRunning = false
    private var tapFormat = AudioStreamBasicDescription()
    private let logger = Logger(subsystem: "com.macaudio.app", category: "systap")

    var onAudioReceived: ((UnsafePointer<AudioBufferList>, UInt32) -> Void)?

    func start() throws {
        guard !isRunning else { return }

        logger.info("Starting system audio capture...")

        // Step 1: Create a process tap for all system audio
        // Use empty exclude list — the API expects Core Audio process AudioObjectIDs,
        // not raw PIDs. Passing PIDs causes !obj error. Empty array captures everything.
        let tapUUID = UUID()
        let tapDesc = CATapDescription(stereoGlobalTapButExcludeProcesses: [])
        tapDesc.uuid = tapUUID
        tapDesc.name = "MacAudio System Tap" as NSString as String
        tapDesc.muteBehavior = .unmuted

        var status = AudioHardwareCreateProcessTap(tapDesc, &tapObjectID)
        guard status == noErr else {
            logger.error("Failed to create process tap: \(status)")
            throw NSError(domain: NSOSStatusErrorDomain, code: Int(status),
                          userInfo: [NSLocalizedDescriptionKey: "System audio capture requires permission. Grant 'Screen & System Audio Recording' in System Settings > Privacy & Security, then restart MacAudio."])
        }
        logger.info("Process tap created: \(self.tapObjectID)")

        // Step 2: Get the default output device UID for the aggregate
        let outputDeviceUID = Self.getDefaultOutputDeviceUID()
        logger.info("Default output device UID: \(outputDeviceUID ?? "none")")

        // Step 3: Create a private aggregate device with the tap
        var aggDict: [String: Any] = [
            kAudioAggregateDeviceUIDKey: UUID().uuidString,
            kAudioAggregateDeviceNameKey: "MacAudio Tap",
            kAudioAggregateDeviceIsPrivateKey: true,
            kAudioAggregateDeviceIsStackedKey: false,
            kAudioAggregateDeviceTapAutoStartKey: true,
            kAudioAggregateDeviceTapListKey: [
                [
                    kAudioSubTapUIDKey: tapUUID.uuidString,
                    kAudioSubTapDriftCompensationKey: true
                ]
            ],
        ]

        // Include default output device as sub-device if available
        if let uid = outputDeviceUID {
            aggDict[kAudioAggregateDeviceMainSubDeviceKey] = uid
            aggDict[kAudioAggregateDeviceSubDeviceListKey] = [
                [kAudioSubDeviceUIDKey: uid]
            ]
        }

        status = AudioHardwareCreateAggregateDevice(aggDict as NSDictionary, &aggregateDeviceID)
        guard status == noErr else {
            logger.error("Failed to create aggregate device: \(status)")
            cleanup()
            throw NSError(domain: NSOSStatusErrorDomain, code: Int(status),
                          userInfo: [NSLocalizedDescriptionKey: "Failed to create aggregate device (status: \(status))"])
        }
        logger.info("Aggregate device created: \(self.aggregateDeviceID)")

        // Step 4: Get the tap's audio format
        var formatAddress = AudioObjectPropertyAddress(
            mSelector: kAudioTapPropertyFormat,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var formatSize = UInt32(MemoryLayout<AudioStreamBasicDescription>.size)
        status = AudioObjectGetPropertyData(tapObjectID, &formatAddress, 0, nil, &formatSize, &tapFormat)
        guard status == noErr else {
            logger.error("Failed to get tap format: \(status)")
            cleanup()
            throw NSError(domain: NSOSStatusErrorDomain, code: Int(status),
                          userInfo: [NSLocalizedDescriptionKey: "Failed to get tap format"])
        }
        logger.info("Tap format: \(self.tapFormat.mSampleRate)Hz, \(self.tapFormat.mChannelsPerFrame)ch, \(self.tapFormat.mBitsPerChannel)bit, bpf=\(self.tapFormat.mBytesPerFrame)")

        // Step 5: Create IOProc on the aggregate device
        var procID: AudioDeviceIOProcID?
        status = AudioDeviceCreateIOProcIDWithBlock(&procID, aggregateDeviceID, nil) {
            [weak self] _, inInputData, _, _, _ in
            guard let self = self else { return }
            let bytesPerFrame = self.tapFormat.mBytesPerFrame
            if bytesPerFrame > 0 {
                let frameCount = inInputData.pointee.mBuffers.mDataByteSize / bytesPerFrame
                self.onAudioReceived?(inInputData, frameCount)
            }
        }

        guard status == noErr, let validProcID = procID else {
            logger.error("Failed to create IOProc: \(status)")
            cleanup()
            throw NSError(domain: NSOSStatusErrorDomain, code: Int(status),
                          userInfo: [NSLocalizedDescriptionKey: "Failed to create IOProc"])
        }
        ioProcID = validProcID

        // Step 6: Start IO
        status = AudioDeviceStart(aggregateDeviceID, ioProcID)
        guard status == noErr else {
            logger.error("Failed to start audio device IO: \(status)")
            cleanup()
            throw NSError(domain: NSOSStatusErrorDomain, code: Int(status),
                          userInfo: [NSLocalizedDescriptionKey: "Failed to start audio device IO"])
        }

        isRunning = true
        logger.info("System audio capture started successfully")
    }

    func stop() {
        guard isRunning else { return }
        isRunning = false
        cleanup()
        logger.info("System audio capture stopped")
    }

    private func cleanup() {
        if let proc = ioProcID, aggregateDeviceID != kAudioObjectUnknown {
            AudioDeviceStop(aggregateDeviceID, proc)
            AudioDeviceDestroyIOProcID(aggregateDeviceID, proc)
            ioProcID = nil
        }
        if aggregateDeviceID != kAudioObjectUnknown {
            AudioHardwareDestroyAggregateDevice(aggregateDeviceID)
            aggregateDeviceID = kAudioObjectUnknown
        }
        if tapObjectID != kAudioObjectUnknown {
            AudioHardwareDestroyProcessTap(tapObjectID)
            tapObjectID = kAudioObjectUnknown
        }
    }

    private static func getDefaultOutputDeviceUID() -> String? {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDefaultOutputDevice,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var deviceID: AudioDeviceID = kAudioObjectUnknown
        var size = UInt32(MemoryLayout<AudioDeviceID>.size)
        let status = AudioObjectGetPropertyData(
            AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size, &deviceID
        )
        guard status == noErr, deviceID != kAudioObjectUnknown else { return nil }

        var uidAddress = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyDeviceUID,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var uid: CFString = "" as CFString
        var uidSize = UInt32(MemoryLayout<CFString>.size)
        let uidStatus = AudioObjectGetPropertyData(deviceID, &uidAddress, 0, nil, &uidSize, &uid)
        guard uidStatus == noErr else { return nil }
        return uid as String
    }

    deinit {
        stop()
    }
}
