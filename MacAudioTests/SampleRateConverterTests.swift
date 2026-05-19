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

        let result = samples.withUnsafeBufferPointer { buf -> (UnsafePointer<Float>, UInt32)? in
            src.convert(buf.baseAddress!, frameCount: frameCount)
        }
        guard let (_, outFrames) = result else { return XCTFail("convert returned nil") }

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

        let result = samples.withUnsafeBufferPointer { buf -> (UnsafePointer<Float>, UInt32)? in
            src.convert(buf.baseAddress!, frameCount: frameCount)
        }
        guard let (_, outFrames) = result else { return XCTFail("convert returned nil") }

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

        let result = samples.withUnsafeBufferPointer { buf -> (UnsafePointer<Float>, UInt32)? in
            src.convert(buf.baseAddress!, frameCount: largeFrameCount)
        }
        XCTAssertNotNil(result, "convert must succeed by reallocating internal buffers")
    }

    func test_convert_preserves_silence_as_silence() {
        guard let src = SampleRateConverter(sourceRate: 48000, destRate: 44100) else {
            return XCTFail("expected SRC to init")
        }
        let frameCount: UInt32 = 512
        let zeros = Array(repeating: Float(0.0), count: Int(frameCount) * 2)

        let result = zeros.withUnsafeBufferPointer { buf -> (UnsafePointer<Float>, UInt32)? in
            src.convert(buf.baseAddress!, frameCount: frameCount)
        }
        guard let (outPtr, outFrames) = result else { return XCTFail("convert returned nil") }

        // The downsample of pure silence must remain effectively silent.
        // Allow for tiny numerical noise from filtering.
        let outSamples = UnsafeBufferPointer(start: outPtr, count: Int(outFrames) * 2)
        for sample in outSamples {
            XCTAssertLessThan(abs(sample), 1e-5, "silent input should remain silent")
        }
    }
}
