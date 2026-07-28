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
        data.append(Data([0x01, 0x02]))  // Length is 100 but body is 2
        XCTAssertThrowsError(try PipelineClient.unpackMessage(data)) { error in
            XCTAssertEqual(error as? PipelineClientError, .incompleteBody)
        }
    }

    // MARK: - feat-06602 C++ Worker Binary Lookup & Error Localization

    func testFindWorkerExecutableFromEnvVar() throws {
        let repoRoot = PipelineClient.findRepoRoot()
        guard let repoRoot else {
            XCTFail("Could not locate repo root")
            return
        }

        let workerBin = repoRoot.appendingPathComponent("build/cpp/bin/sublift_worker").path
        guard FileManager.default.isExecutableFile(atPath: workerBin) else {
            throw XCTSkip("sublift_worker binary not compiled in build/cpp/bin/")
        }

        let env = ["SUBLIFT_WORKER_PATH": workerBin]
        let found = try PipelineClient.findWorkerExecutable(envOverride: env)
        XCTAssertEqual(found, workerBin)
    }

    func testFindWorkerExecutableFromRepoBuildDir() throws {
        let repoRoot = PipelineClient.findRepoRoot()
        guard let repoRoot else {
            XCTFail("Could not locate repo root")
            return
        }

        let workerBin = repoRoot.appendingPathComponent("build/cpp/bin/sublift_worker").path
        guard FileManager.default.isExecutableFile(atPath: workerBin) else {
            throw XCTSkip("sublift_worker binary not compiled in build/cpp/bin/")
        }

        let found = try PipelineClient.findWorkerExecutable(envOverride: [:])
        XCTAssertTrue(found.hasSuffix("sublift_worker"))
    }

    func testFindWorkerExecutableNotFoundThrowsError() {
        class MockFileManager: FileManager {
            override func fileExists(atPath path: String, isDirectory: UnsafeMutablePointer<ObjCBool>?) -> Bool {
                return false
            }
            override func isExecutableFile(atPath path: String) -> Bool {
                return false
            }
        }

        let mockFM = MockFileManager()
        let env = ["SUBLIFT_WORKER_PATH": "/nonexistent/fake/sublift_worker"]

        XCTAssertThrowsError(
            try PipelineClient.findWorkerExecutable(envOverride: env, fileManager: mockFM)
        ) { error in
            guard case PipelineClientError.workerBinaryNotFound(let searched) = error else {
                XCTFail("Expected workerBinaryNotFound error")
                return
            }
            XCTAssertTrue(searched.contains("/nonexistent/fake/sublift_worker"))
        }
    }

    func testErrorLocalizationForNewErrorTypes() {
        let err1 = PipelineClientError.workerBinaryNotFound(searchedPaths: ["/tmp/worker"])
        XCTAssertTrue(err1.localizedDescription.contains("未找到 SubLift C++ Worker 可执行文件"))

        let err2 = PipelineClientError.engineMismatch(requested: "paddle", supported: ["mock", "vision"])
        XCTAssertTrue(err2.localizedDescription.contains("请求的引擎 'paddle' 不被当前 Worker 支持"))
    }
}
