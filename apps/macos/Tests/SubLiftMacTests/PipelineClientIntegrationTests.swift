import XCTest
@testable import SubLiftMac

/// 跨进程集成测试：Swift PipelineClient 启动真实 Python 子进程并握手。
///
/// 标记为集成测试，CI 跳过（需要 Python 环境）。
/// 本地运行：`swift test --filter PipelineClientIntegrationTests`
final class PipelineClientIntegrationTests: XCTestCase {

    /// Python 解释器路径（项目根 .venv/bin/python）。
    private let pythonPath = "/Volumes/lab/pp/SubLift/.venv/bin/python"

    func testStartAndHandshakeWithPythonServer() throws {
        let client = PipelineClient()
        defer { client.stop() }

        do {
            let success = try client.start(pythonExecutable: pythonPath)
            XCTAssertTrue(success, "握手应成功：发 hello 收 bye")
        } catch let error as PipelineClientError {
            XCTFail("PipelineClient 失败: \(error)")
        }
    }

    func testStopCleansUpResources() throws {
        let client = PipelineClient()
        try client.start(pythonExecutable: pythonPath)

        let socketPath = client.socketPath
        XCTAssertFalse(socketPath.isEmpty)

        client.stop()

        XCTAssertTrue(client.socketPath.isEmpty, "stop 后 socketPath 应清空")
        XCTAssertFalse(FileManager.default.fileExists(atPath: socketPath), "stop 后 socket 文件应删除")
    }
}
