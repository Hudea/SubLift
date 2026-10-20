import Foundation
import Testing
@testable import SubLiftMac

private struct SrtFixtureCase: Decodable {
    let name: String
    let entries: [SubtitleEntryData]
    let expected: String
}

private func sharedSrtFixtureURL() -> URL {
    var root = URL(fileURLWithPath: #filePath)
    for _ in 0..<5 {
        root.deleteLastPathComponent()
    }
    return root.appendingPathComponent("contracts/fixtures/srt-cases.json")
}

/// Phase 17 D02：Swift formatter 必须与共享 SRT fixtures 逐字节一致。
struct SrtContractTests {

    @Test
    func sharedSrtFixturesMatch() throws {
        let data = try Data(contentsOf: sharedSrtFixtureURL())
        let cases = try JSONDecoder().decode([SrtFixtureCase].self, from: data)
        #expect(!cases.isEmpty)

        for testCase in cases {
            let actual = SrtFormatter.format(entries: testCase.entries)
            #expect(actual == testCase.expected, "fixture: \(testCase.name)")
        }
    }
}
