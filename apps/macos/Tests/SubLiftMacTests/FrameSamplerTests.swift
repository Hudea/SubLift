import CoreImage
import Foundation
import ImageIO
import Testing
@testable import SubLiftMac

/// feat-018：FrameSampler 纯函数单测（JPEG 编码）。
///
/// AVAssetReader 抽帧行为靠手动验收（需真实视频文件 + GUI）。
/// 这里只测可纯函数化的 encodeJPEG。
struct FrameSamplerTests {

    // MARK: - 辅助

    /// 构造纯色测试 CGImage。
    private func makeTestImage(width: Int, height: Int, color: [CGFloat]) -> CGImage? {
        let cs = CGColorSpace(name: CGColorSpace.sRGB)!
        let info = CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedLast.rawValue)
        guard let ctx = CGContext(
            data: nil,
            width: width,
            height: height,
            bitsPerComponent: 8,
            bytesPerRow: 0,
            space: cs,
            bitmapInfo: info.rawValue
        ) else { return nil }
        ctx.setFillColor(color)
        ctx.fill(CGRect(x: 0, y: 0, width: width, height: height))
        return ctx.makeImage()
    }

    // MARK: - encodeJPEG

    @Test
    func encodeJPEGReturnsValidData() throws {
        let image = try #require(makeTestImage(width: 100, height: 100, color: [1, 0, 0, 1]))
        let data = FrameSampler.encodeJPEG(image, quality: 0.85)
        #expect(data != nil)
        #expect(data!.count > 0)
    }

    @Test
    func encodeJPEGStartsWithJFIFMagicBytes() throws {
        let image = try #require(makeTestImage(width: 10, height: 10, color: [0, 1, 0, 1]))
        let data = try #require(FrameSampler.encodeJPEG(image, quality: 0.85))
        // JPEG SOI marker: FF D8
        #expect(data[0] == 0xFF)
        #expect(data[1] == 0xD8)
    }

    @Test
    func encodeJPEGQualityAffectsSize() throws {
        let image = try #require(makeTestImage(width: 200, height: 200, color: [0.5, 0.5, 0.5, 1]))
        let highQ = try #require(FrameSampler.encodeJPEG(image, quality: 1.0))
        let lowQ = try #require(FrameSampler.encodeJPEG(image, quality: 0.1))
        // 高质量 JPEG 通常比低质量大（对复杂图像更明显，纯色图差异可能不大）
        #expect(highQ.count >= lowQ.count)
    }

    @Test
    func encodeJPEGProducesValidImage() throws {
        let image = try #require(makeTestImage(width: 50, height: 50, color: [0, 0, 1, 1]))
        let data = try #require(FrameSampler.encodeJPEG(image, quality: 0.85))
        // 验证可以重新解码
        let cfData = data as CFData
        let source = CGImageSourceCreateWithData(cfData, nil)
        #expect(source != nil)
        let count = CGImageSourceGetCount(source!)
        #expect(count == 1)
        let status = CGImageSourceGetStatusAtIndex(source!, 0)
        #expect(status == .statusComplete)
    }

    // MARK: - Config

    @Test
    func configDefaultValues() {
        let config = FrameSampler.Config.default
        #expect(config.fps == 5)
        #expect(config.jpegQuality == 0.85)
    }

    @Test
    func configCustomValues() {
        let config = FrameSampler.Config(fps: 10, jpegQuality: 0.5)
        #expect(config.fps == 10)
        #expect(config.jpegQuality == 0.5)
    }
}
