import Foundation
import Testing
@testable import SubLiftMac

/// feat-019：MjpegParser SOI/EOI 切帧纯函数单测。
struct FfmpegFallbackTests {

    /// 构造模拟 JPEG 帧：FF D8 + body + FF D9
    private func makeJPEG(body: [UInt8] = [0x00, 0x01, 0x02]) -> Data {
        var data = Data([0xFF, 0xD8])  // SOI
        data.append(contentsOf: body)
        data.append(contentsOf: [0xFF, 0xD9])  // EOI
        return data
    }

    // MARK: - MjpegParser.parseNextJPEG

    @Test
    func parseSingleCompleteJPEG() {
        var buffer = makeJPEG()
        let result = MjpegParser.parseNextJPEG(from: &buffer)
        #expect(result != nil)
        #expect(result?.count == 7)  // 2 (SOI) + 3 (body) + 2 (EOI)
        #expect(buffer.isEmpty)
    }

    @Test
    func parseMultipleJPEGs() {
        var buffer = Data()
        buffer.append(makeJPEG(body: [0xAA]))
        buffer.append(makeJPEG(body: [0xBB, 0xCC]))
        buffer.append(makeJPEG(body: [0xDD]))

        let frame1 = MjpegParser.parseNextJPEG(from: &buffer)
        let frame2 = MjpegParser.parseNextJPEG(from: &buffer)
        let frame3 = MjpegParser.parseNextJPEG(from: &buffer)

        #expect(frame1?.count == 5)   // 2 + 1 + 2
        #expect(frame2?.count == 6)   // 2 + 2 + 2
        #expect(frame3?.count == 5)   // 2 + 1 + 2
        #expect(buffer.isEmpty)
    }

    @Test
    func incompleteFrameReturnsNil() {
        var buffer = Data([0xFF, 0xD8, 0x00, 0x01])
        let result = MjpegParser.parseNextJPEG(from: &buffer)
        #expect(result == nil)
        #expect(buffer.count == 4)
    }

    @Test
    func emptyBufferReturnsNil() {
        var buffer = Data()
        let result = MjpegParser.parseNextJPEG(from: &buffer)
        #expect(result == nil)
        #expect(buffer.isEmpty)
    }

    @Test
    func garbageBeforeSOIIsDiscarded() {
        var buffer = Data()
        buffer.append(contentsOf: [0x00, 0x01, 0x02, 0x03])
        buffer.append(makeJPEG(body: [0xAA]))

        let result = MjpegParser.parseNextJPEG(from: &buffer)
        #expect(result?.count == 5)
        #expect(buffer.isEmpty)
    }

    @Test
    func noSOIReturnsNil() {
        var buffer = Data([0x00, 0x01, 0x02, 0xFF, 0xD9, 0x03])
        let result = MjpegParser.parseNextJPEG(from: &buffer)
        #expect(result == nil)
    }

    @Test
    func partialSOIAtEndPreserved() {
        var buffer = Data([0x00, 0x01, 0xFF])
        let result = MjpegParser.parseNextJPEG(from: &buffer)
        #expect(result == nil)
        #expect(buffer.count == 1)
        #expect(buffer.first == 0xFF)
    }

    @Test
    func frameWithFFInBody() {
        var buffer = makeJPEG(body: [0xFF, 0x00, 0xAA])
        let result = MjpegParser.parseNextJPEG(from: &buffer)
        #expect(result?.count == 7)
        #expect(buffer.isEmpty)
    }

    @Test
    func parseAndCheckSOIMagic() {
        var buffer = makeJPEG()
        let result = MjpegParser.parseNextJPEG(from: &buffer)
        #expect(result?.first == 0xFF)
        #expect(result?.dropFirst().first == 0xD8)
    }

    @Test
    func parseAndCheckEOIMagic() {
        var buffer = makeJPEG()
        let result = MjpegParser.parseNextJPEG(from: &buffer)
        #expect(result?.last == 0xD9)
        #expect(result?.dropLast().last == 0xFF)
    }
}
