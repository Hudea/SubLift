import XCTest
@testable import SubLiftMac

final class PipelineClientTests: XCTestCase {

    // MARK: - packMessage

    func testPackMessageFormat() throws {
        let message: [String: Any] = ["type": "bye"]
        let data = try PipelineClient.packMessage(message)

        // body = {"type":"bye"} = 14 bytes
        XCTAssertEqual(data.count, PipelineClient.lengthPrefixSize + 14)

        let length = data.subdata(in: 0..<4).withUnsafeBytes { ptr in
            ptr.load(as: UInt32.self).bigEndian
        }
        XCTAssertEqual(length, 14)

        let body = data.subdata(in: 4..<18)
        let json = try JSONSerialization.jsonObject(with: body) as? [String: String]
        XCTAssertEqual(json?["type"], "bye")
    }

    func testPackMessageHello() throws {
        let message: [String: Any] = ["type": "hello", "client": "sublift-mac"]
        let data = try PipelineClient.packMessage(message)

        let length = data.subdata(in: 0..<4).withUnsafeBytes { ptr in
            ptr.load(as: UInt32.self).bigEndian
        }
        let body = data.subdata(in: 4..<(4 + Int(length)))

        let json = try JSONSerialization.jsonObject(with: body) as? [String: String]
        XCTAssertEqual(json?["type"], "hello")
        XCTAssertEqual(json?["client"], "sublift-mac")
    }

    // MARK: - unpackMessage

    func testUnpackMessageBye() throws {
        let body = #"{"type":"bye"}"#.data(using: .utf8)!
        var length = UInt32(body.count).bigEndian
        var data = Data()
        withUnsafeBytes(of: &length) { ptr in
            data.append(contentsOf: ptr)
        }
        data.append(body)

        let message = try PipelineClient.unpackMessage(data)
        XCTAssertEqual(message?["type"] as? String, "bye")
    }

    func testUnpackMessageHello() throws {
        let body = #"{"client":"sublift-mac","type":"hello"}"#.data(using: .utf8)!
        var length = UInt32(body.count).bigEndian
        var data = Data()
        withUnsafeBytes(of: &length) { ptr in
            data.append(contentsOf: ptr)
        }
        data.append(body)

        let message = try PipelineClient.unpackMessage(data)
        XCTAssertEqual(message?["type"] as? String, "hello")
        XCTAssertEqual(message?["client"] as? String, "sublift-mac")
    }

    func testUnpackEmptyDataReturnsNil() throws {
        let data = Data()
        let message = try PipelineClient.unpackMessage(data)
        XCTAssertNil(message)
    }

    // MARK: - Roundtrip

    func testPackUnpackRoundtrip() throws {
        let original: [String: Any] = [
            "type": "hello",
            "client": "sublift-mac",
            "version": 1,
        ]
        let packed = try PipelineClient.packMessage(original)
        let unpacked = try PipelineClient.unpackMessage(packed)

        XCTAssertEqual(unpacked?["type"] as? String, "hello")
        XCTAssertEqual(unpacked?["client"] as? String, "sublift-mac")
        XCTAssertEqual(unpacked?["version"] as? Int, 1)
    }

    // MARK: - Error cases

    func testUnpackIncompleteLengthPrefixThrows() {
        let data = Data([0x00, 0x00])  // Only 2 bytes, need 4
        XCTAssertThrowsError(try PipelineClient.unpackMessage(data)) { error in
            XCTAssertEqual(error as? PipelineClientError, .incompleteLengthPrefix)
        }
    }

    func testUnpackIncompleteBodyThrows() {
        var length = UInt32(100).bigEndian
        var data = Data()
        withUnsafeBytes(of: &length) { ptr in
            data.append(contentsOf: ptr)
        }
        data.append(Data([0x01, 0x02]))

        XCTAssertThrowsError(try PipelineClient.unpackMessage(data)) { error in
            XCTAssertEqual(error as? PipelineClientError, .incompleteBody)
        }
    }

    // MARK: - LocalizedError

    func testServerErrorLocalizedMessagePreservesPythonMessage() throws {
        // feat-05004 P1#4：serverError 携带的 Python 端消息（如 uv sync / 网络错误）
        // 必须通过 errorDescription 透传，不得退化为系统默认文案。
        let message = "PaddleOCR 不可用。请安装可选依赖：uv sync --extra paddle"
        let error = PipelineClientError.serverError(message)
        XCTAssertEqual(error.errorDescription, message)
        XCTAssertTrue(error.localizedDescription.contains("uv sync --extra paddle"))
    }

    func testAllErrorsHaveLocalizedDescription() throws {
        // 所有 case 都应给出可读 errorDescription，不返回 nil / 通用文案。
        let cases: [PipelineClientError] = [
            .incompleteLengthPrefix,
            .incompleteBody,
            .serverStartTimeout,
            .socketCreateFailed(1),
            .socketConnectFailed(2),
            .socketWriteFailed(3),
            .serverError("boom"),
            .connectionClosed,
        ]
        for c in cases {
            XCTAssertNotNil(c.errorDescription)
            XCTAssertFalse(c.errorDescription?.isEmpty ?? true)
        }
    }
}
