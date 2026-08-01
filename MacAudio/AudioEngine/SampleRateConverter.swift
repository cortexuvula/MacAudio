import AVFAudio
import os

final class SampleRateConverter {
    private let converter: AVAudioConverter
    private let inputFormat: AVAudioFormat
    private let outputFormat: AVAudioFormat

    /// Source sample rate (Hz). Used by callers to size the output buffer.
    var inputRate: Float64 { inputFormat.sampleRate }
    /// Destination sample rate (Hz).
    var outputRate: Float64 { outputFormat.sampleRate }
    // Reused across calls to avoid per-callback allocation on the realtime
    // thread. Owned exclusively by whichever thread is calling convert() —
    // callers must not share one instance across threads. (AudioMixer keeps one
    // instance per capture source, each driven from its own IO thread.)
    private var inputPCMBuffer: AVAudioPCMBuffer
    private var outputPCMBuffer: AVAudioPCMBuffer
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

    /// Converts interleaved Float32 frames into the caller-supplied output
    /// buffer. Returns the number of frames written (0 on failure).
    ///
    /// The output buffer must point to at least `outputCapacity` Float slots.
    /// Writing into a caller-owned buffer (rather than returning a pointer into
    /// this object's internal `outputPCMBuffer`) eliminates the escaping-pointer
    /// hazard where a previously-returned pointer would alias storage that a
    /// later call could overwrite or realloc.
    func convert(_ input: UnsafePointer<Float>, frameCount: UInt32,
                 into output: UnsafeMutablePointer<Float>, outputCapacity: UInt32) -> UInt32 {
        let channels = inputFormat.channelCount

        // Grow the reusable input/output PCM buffers if this call exceeds their
        // capacity (rare). They back the AVAudioConverter interaction only for
        // the duration of this call.
        if frameCount > inputPCMBuffer.frameCapacity {
            let ratio = outputFormat.sampleRate / inputFormat.sampleRate
            let newOutCapacity = AVAudioFrameCount(ceil(Double(frameCount) * ratio)) + 1
            guard let newInBuf = AVAudioPCMBuffer(pcmFormat: inputFormat, frameCapacity: frameCount),
                  let newOutBuf = AVAudioPCMBuffer(pcmFormat: outputFormat, frameCapacity: newOutCapacity) else {
                return 0
            }
            inputPCMBuffer = newInBuf
            outputPCMBuffer = newOutBuf
            logger.warning("SRC: input exceeded maxFrames (\(frameCount)), reallocating")
        }

        let inBuf = inputPCMBuffer
        let outBuf = outputPCMBuffer

        let sampleCount = Int(frameCount) * Int(channels)
        guard let inData = inBuf.floatChannelData?[0] else { return 0 }
        inData.update(from: input, count: sampleCount)
        inBuf.frameLength = frameCount
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
            return 0
        }

        guard outBuf.frameLength > 0,
              let outData = outBuf.floatChannelData?[0] else {
            return 0
        }

        // Now that the actual output length is known, verify the caller's
        // buffer can hold it. (Checking against outBuf.frameCapacity up front
        // would wrongly reject smaller per-call conversions whose actual output
        // is well within capacity.)
        let writtenSamples = Int(outBuf.frameLength) * Int(channels)
        guard Int(outputCapacity) >= writtenSamples else {
            logger.error("SRC: output capacity \(outputCapacity) < written \(writtenSamples)")
            return 0
        }
        output.update(from: outData, count: writtenSamples)
        return outBuf.frameLength
    }
}
