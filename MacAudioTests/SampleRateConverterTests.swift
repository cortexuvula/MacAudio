import XCTest
@testable import MacAudio

final class SampleRateConverterTests: XCTestCase {

    func test_init_returns_nil_when_source_and_dest_rates_match() {
        XCTAssertNil(SampleRateConverter(sourceRate: 48000, destRate: 48000))
        XCTAssertNil(SampleRateConverter(sourceRate: 44100, destRate: 44100))
    }

    func test_init_succeeds_for_supported_rate_pairs() {
        XCTAssertNotNil(SampleRateConverter(sourceRate: 44100, destRate: 48000))
        XCTAssertNotNil(SampleRateConverter(sourceRate: 48000, destRate: 44100))
        XCTAssertNotNil(SampleRateConverter(sourceRate: 96000, destRate: 48000))
        XCTAssertNotNil(SampleRateConverter(sourceRate: 48000, destRate: 96000))
    }

    func test_upsample_produces_more_frames_than_input() {
        guard let src = SampleRateConverter(sourceRate: 48000, destRate: 96000) else {
            return XCTFail("expected SRC to init")
        }
        let frameCount: UInt32 = 1024
        let samples = Array(repeating: Float(0.0), count: Int(frameCount) * 2)

        // Worst-case output capacity for a 2x upsample.
        let outCapacity = UInt32(Int(frameCount) * 2 * 2 + 2)
        var out = [Float](repeating: 0, count: Int(outCapacity))
        let outFrames = samples.withUnsafeBufferPointer { buf -> UInt32 in
            out.withUnsafeMutableBufferPointer { outBuf in
                src.convert(buf.baseAddress!, frameCount: frameCount,
                            into: outBuf.baseAddress!, outputCapacity: outCapacity)
            }
        }
        XCTAssertGreaterThan(outFrames, 0, "convert should produce output")

        // 48k → 96k: expect roughly 2x frames. AVAudioConverter may emit
        // a few extra/fewer at the boundary; assert "close to 2x" with
        // generous slack rather than exact.
        XCTAssertGreaterThan(outFrames, frameCount, "upsample should produce more frames")
        XCTAssertLessThan(outFrames, frameCount * 3, "upsample should not balloon output")
    }

    func test_downsample_produces_fewer_frames_than_input() {
        guard let src = SampleRateConverter(sourceRate: 96000, destRate: 48000) else {
            return XCTFail("expected SRC to init")
        }
        let frameCount: UInt32 = 2048
        let samples = Array(repeating: Float(0.0), count: Int(frameCount) * 2)

        let outCapacity = frameCount * 2
        var out = [Float](repeating: 0, count: Int(outCapacity))
        let outFrames = samples.withUnsafeBufferPointer { buf -> UInt32 in
            out.withUnsafeMutableBufferPointer { outBuf in
                src.convert(buf.baseAddress!, frameCount: frameCount,
                            into: outBuf.baseAddress!, outputCapacity: outCapacity)
            }
        }
        XCTAssertGreaterThan(outFrames, 0, "convert should produce output")

        XCTAssertLessThan(outFrames, frameCount, "downsample should produce fewer frames")
        XCTAssertGreaterThan(outFrames, frameCount / 4, "downsample should not collapse output")
    }

    func test_convert_handles_frame_count_exceeding_max_frames() {
        // Init with a small maxFrames; convert with a larger frameCount.
        // The converter must reallocate its internal buffers rather than fail.
        guard let src = SampleRateConverter(sourceRate: 48000, destRate: 44100, maxFrames: 256) else {
            return XCTFail("expected SRC to init")
        }
        let largeFrameCount: UInt32 = 1024
        let samples = Array(repeating: Float(0.1), count: Int(largeFrameCount) * 2)

        let outCapacity = largeFrameCount * 2
        var out = [Float](repeating: 0, count: Int(outCapacity))
        let outFrames = samples.withUnsafeBufferPointer { buf -> UInt32 in
            out.withUnsafeMutableBufferPointer { outBuf in
                src.convert(buf.baseAddress!, frameCount: largeFrameCount,
                            into: outBuf.baseAddress!, outputCapacity: outCapacity)
            }
        }
        XCTAssertGreaterThan(outFrames, 0, "convert must succeed by reallocating internal buffers")
    }

    func test_convert_preserves_silence_as_silence() {
        guard let src = SampleRateConverter(sourceRate: 48000, destRate: 44100) else {
            return XCTFail("expected SRC to init")
        }
        let frameCount: UInt32 = 512
        let zeros = Array(repeating: Float(0.0), count: Int(frameCount) * 2)

        let outCapacity = frameCount * 2
        var out = [Float](repeating: 0, count: Int(outCapacity))
        let outFrames = zeros.withUnsafeBufferPointer { buf -> UInt32 in
            out.withUnsafeMutableBufferPointer { outBuf in
                src.convert(buf.baseAddress!, frameCount: frameCount,
                            into: outBuf.baseAddress!, outputCapacity: outCapacity)
            }
        }
        XCTAssertGreaterThan(outFrames, 0, "convert should produce output")

        // The downsample of pure silence must remain effectively silent.
        // Allow for tiny numerical noise from filtering.
        for i in 0..<Int(outFrames) * 2 {
            XCTAssertLessThan(abs(out[i]), 1e-5, "silent input should remain silent")
        }
    }

    func test_convert_writes_into_caller_buffer_not_internal() {
        // Regression guard for the escaping-pointer hazard: the caller-owned
        // buffer must hold the exact output and remain valid independent of the
        // converter's internal state. A second call with different input must
        // not retroactively change the first output.
        guard let src = SampleRateConverter(sourceRate: 48000, destRate: 96000) else {
            return XCTFail("expected SRC to init")
        }
        let frameCount: UInt32 = 256
        let loud = Array(repeating: Float(0.5), count: Int(frameCount) * 2)
        let outCapacity = UInt32(Int(frameCount) * 2 * 2 + 2)

        var firstOut = [Float](repeating: 0, count: Int(outCapacity))
        let firstFrames = loud.withUnsafeBufferPointer { buf -> UInt32 in
            firstOut.withUnsafeMutableBufferPointer { outBuf in
                src.convert(buf.baseAddress!, frameCount: frameCount,
                            into: outBuf.baseAddress!, outputCapacity: outCapacity)
            }
        }
        XCTAssertGreaterThan(firstFrames, 0)
        // Snapshot what the caller's buffer holds after call 1.
        let snapshot = Array(firstOut.prefix(Int(firstFrames) * 2))

        // Second, independent call with silence into a different buffer.
        let silent = Array(repeating: Float(0.0), count: Int(frameCount) * 2)
        var secondOut = [Float](repeating: 0, count: Int(outCapacity))
        _ = silent.withUnsafeBufferPointer { buf -> UInt32 in
            secondOut.withUnsafeMutableBufferPointer { outBuf in
                src.convert(buf.baseAddress!, frameCount: frameCount,
                            into: outBuf.baseAddress!, outputCapacity: outCapacity)
            }
        }

        // The first caller buffer must be unaffected by the second call.
        for i in 0..<Int(firstFrames) * 2 {
            XCTAssertEqual(firstOut[i], snapshot[i], "caller buffer must not be mutated by a later call")
        }
    }
}
