import AVFAudio
import os

final class SampleRateConverter {
    private let converter: AVAudioConverter
    private let inputFormat: AVAudioFormat
    private let outputFormat: AVAudioFormat
    private var inputPCMBuffer: AVAudioPCMBuffer
    private let outputPCMBuffer: AVAudioPCMBuffer
    private let logger = Logger(subsystem: "com.macaudio.app", category: "src")

    /// Returns nil if sourceRate == destRate (no conversion needed).
    /// Configured for interleaved stereo Float32.
    init?(sourceRate: Float64, destRate: Float64, channels: UInt32 = 2,
          maxFrames: AVAudioFrameCount = 4096) {
        guard sourceRate != destRate else { return nil }

        guard let inFmt = AVAudioFormat(commonFormat: .pcmFormatFloat32,
                                        sampleRate: sourceRate,
                                        channels: channels,
                                        interleaved: true),
              let outFmt = AVAudioFormat(commonFormat: .pcmFormatFloat32,
                                         sampleRate: destRate,
                                         channels: channels,
                                         interleaved: true) else {
            return nil
        }

        guard let conv = AVAudioConverter(from: inFmt, to: outFmt) else {
            return nil
        }

        let ratio = destRate / sourceRate
        let outFrameCapacity = AVAudioFrameCount(ceil(Double(maxFrames) * ratio)) + 1

        guard let inBuf = AVAudioPCMBuffer(pcmFormat: inFmt, frameCapacity: maxFrames),
              let outBuf = AVAudioPCMBuffer(pcmFormat: outFmt, frameCapacity: outFrameCapacity) else {
            return nil
        }

        self.inputFormat = inFmt
        self.outputFormat = outFmt
        self.converter = conv
        self.inputPCMBuffer = inBuf
        self.outputPCMBuffer = outBuf

        logger.info("SRC enabled: \(sourceRate)Hz -> \(destRate)Hz, \(channels)ch, maxFrames=\(maxFrames)")
    }

    /// Converts interleaved Float32 frames. Returns (pointer, frameCount) valid until next call.
    func convert(_ input: UnsafePointer<Float>, frameCount: UInt32) -> (UnsafePointer<Float>, UInt32)? {
        let channels = inputFormat.channelCount

        // Reallocate input buffer if needed (rare)
        if frameCount > inputPCMBuffer.frameCapacity {
            let ratio = outputFormat.sampleRate / inputFormat.sampleRate
            let newOutCapacity = AVAudioFrameCount(ceil(Double(frameCount) * ratio)) + 1
            guard let newInBuf = AVAudioPCMBuffer(pcmFormat: inputFormat, frameCapacity: frameCount),
                  let newOutBuf = AVAudioPCMBuffer(pcmFormat: outputFormat, frameCapacity: newOutCapacity) else {
                return nil
            }
            inputPCMBuffer = newInBuf
            // Can't reassign let, so we handle this differently
            logger.warning("SRC: input exceeded maxFrames (\(frameCount)), reallocating")
            return convertWithBuffers(input, frameCount: frameCount, inBuf: newInBuf, outBuf: newOutBuf)
        }

        return convertWithBuffers(input, frameCount: frameCount, inBuf: inputPCMBuffer, outBuf: outputPCMBuffer)
    }

    private func convertWithBuffers(_ input: UnsafePointer<Float>, frameCount: UInt32,
                                    inBuf: AVAudioPCMBuffer, outBuf: AVAudioPCMBuffer) -> (UnsafePointer<Float>, UInt32)? {
        let channels = inputFormat.channelCount
        let sampleCount = Int(frameCount) * Int(channels)

        // Copy input data into the PCM buffer
        guard let inData = inBuf.floatChannelData?[0] else { return nil }
        inData.update(from: input, count: sampleCount)
        inBuf.frameLength = frameCount

        // Reset output
        outBuf.frameLength = 0

        var error: NSError?
        var inputConsumed = false
        let status = converter.convert(to: outBuf, error: &error) { _, outStatus in
            if inputConsumed {
                outStatus.pointee = .noDataNow
                return nil
            }
            inputConsumed = true
            outStatus.pointee = .haveData
            return inBuf
        }

        guard status != .error, error == nil else {
            logger.error("SRC convert error: \(error?.localizedDescription ?? "unknown")")
            return nil
        }

        guard outBuf.frameLength > 0,
              let outData = outBuf.floatChannelData?[0] else {
            return nil
        }

        return (UnsafePointer(outData), outBuf.frameLength)
    }
}
