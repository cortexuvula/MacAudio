import Foundation

enum AudioConstants {
    static let shmName = "/macaudio_ringbuffer"
    static let ringBufferFrames: UInt32 = 16384
    static let numChannels: UInt32 = 2
    static let maxFrameSize: Int = Int(ringBufferFrames) * Int(numChannels)
    static let defaultSampleRate: Float64 = 48000.0
    static let supportedSampleRates: [Float64] = [44100.0, 48000.0, 96000.0]
    static let virtualDeviceUID = "MacAudioDevice_UID"
    static let defaultGain: Float = 0.7
}
