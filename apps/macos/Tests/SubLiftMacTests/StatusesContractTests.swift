import Foundation
import Testing
@testable import SubLiftMac

/// Phase 17 D02：批量任务状态必须与 contracts/fixtures/statuses.json 同一集合。
@Suite("Statuses contract")
struct StatusesContractTests {
    @Test func taskStatusesMatchContractFixture() throws {
        let repoRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()  // SubLiftMacTests
            .deletingLastPathComponent()  // Tests
            .deletingLastPathComponent()  // macos
            .deletingLastPathComponent()  // apps
            .deletingLastPathComponent()  // repo root
        let fixtureURL = repoRoot.appendingPathComponent(
            "contracts/fixtures/statuses.json"
        )
        let data = try Data(contentsOf: fixtureURL)
        let root = try #require(
            try JSONSerialization.jsonObject(with: data) as? [String: Any]
        )
        let task = try #require(root["task"] as? [String])
        let fixtureStatuses = Set(task)
        let swiftStatuses = Set(BatchTaskStatus.allCases.map(\.rawValue))
        #expect(fixtureStatuses == swiftStatuses)
    }
}
