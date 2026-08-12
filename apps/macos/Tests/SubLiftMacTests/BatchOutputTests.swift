import XCTest
@testable import SubLiftMac

// MARK: - 输出规划（08104）

final class BatchOutputPlannerTests: XCTestCase {

    private var tempDir: URL!
    private let fileManager = FileManager.default

    override func setUpWithError() throws {
        tempDir = fileManager.temporaryDirectory
            .appendingPathComponent("sublift-planner-\(UUID().uuidString)", isDirectory: true)
        try fileManager.createDirectory(at: tempDir, withIntermediateDirectories: true)
    }

    override func tearDownWithError() throws {
        try? fileManager.setAttributes([.posixPermissions: 0o755], ofItemAtPath: tempDir.path)
        try fileManager.removeItem(at: tempDir)
    }

    private func makeFile(_ name: String, in dir: URL? = nil) throws -> URL {
        let target = (dir ?? tempDir).appendingPathComponent(name)
        if !fileManager.fileExists(atPath: target.deletingLastPathComponent().path) {
            try fileManager.createDirectory(at: target.deletingLastPathComponent(), withIntermediateDirectories: true)
        }
        try Data("test".utf8).write(to: target)
        return target
    }

    // MARK: - sidecar 默认

    func testSidecarDefaultSameDirectoryBasename() throws {
        let source = try makeFile("movie.mp4")
        let plan = try BatchOutputPlanner().plan(source: source, sourceRoot: nil)
        XCTAssertEqual(plan.targetURL.lastPathComponent, "movie.srt")
        XCTAssertEqual(plan.targetURL.deletingLastPathComponent(), tempDir.standardizedFileURL)
        XCTAssertEqual(plan.conflict, .none)
    }

    // MARK: - 公共输出根

    func testPublicRootKeepsRelativeDirectoryForFolderInput() throws {
        let root = tempDir.appendingPathComponent("out", isDirectory: true)
        try fileManager.createDirectory(at: root, withIntermediateDirectories: true)
        let source = try makeFile("sub/deep/movie.mp4")
        let sourceRoot = tempDir.appendingPathComponent("sub", isDirectory: true)
        let planner = BatchOutputPlanner(outputRoot: root)
        let plan = try planner.plan(source: source, sourceRoot: sourceRoot)
        XCTAssertEqual(plan.targetURL.path, root.appendingPathComponent("deep/movie.srt").path)
    }

    func testPublicRootUsesBasenameForStandaloneFile() throws {
        let root = tempDir.appendingPathComponent("out", isDirectory: true)
        try fileManager.createDirectory(at: root, withIntermediateDirectories: true)
        let source = try makeFile("movie.mp4")
        let plan = try BatchOutputPlanner(outputRoot: root).plan(source: source, sourceRoot: nil)
        XCTAssertEqual(plan.targetURL.path, root.appendingPathComponent("movie.srt").path)
    }

    func testPathEscapeRejectedFailClosed() throws {
        let root = tempDir.appendingPathComponent("out", isDirectory: true)
        try fileManager.createDirectory(at: root, withIntermediateDirectories: true)
        // 源在 sourceRoot 之外（sourceRoot 是无关路径）：无法保持相对目录 → fail-closed。
        let outside = tempDir.appendingPathComponent("outside", isDirectory: true)
        try fileManager.createDirectory(at: outside, withIntermediateDirectories: true)
        let source = try makeFile("movie.mp4", in: outside)
        let planner = BatchOutputPlanner(outputRoot: root)
        XCTAssertThrowsError(try planner.plan(
            source: source,
            sourceRoot: tempDir.appendingPathComponent("elsewhere", isDirectory: true)
        )) { error in
            guard case BatchOutputPlannerError.targetOutsideRoot = error else {
                return XCTFail("应拒绝逃逸目标，实际 \(error)")
            }
        }
    }

    // MARK: - 冲突策略

    func testConflictDefaultSkip() throws {
        let source = try makeFile("movie.mp4")
        let existing = try makeFile("movie.srt")
        let plan = try BatchOutputPlanner().plan(source: source, sourceRoot: nil)
        XCTAssertEqual(plan.targetURL, existing.standardizedFileURL)
        XCTAssertEqual(plan.conflict, .skipped(existingURL: existing.standardizedFileURL))
    }

    func testRenamePicksFirstStableAvailableName() throws {
        let source = try makeFile("movie.mp4")
        _ = try makeFile("movie.srt")
        _ = try makeFile("movie (1).srt")
        let planner = BatchOutputPlanner(conflictPolicy: .rename)
        let plan = try planner.plan(source: source, sourceRoot: nil)
        XCTAssertEqual(plan.targetURL.lastPathComponent, "movie (2).srt")
        XCTAssertEqual(plan.conflict, .renamed(
            from: tempDir.appendingPathComponent("movie.srt").standardizedFileURL,
            to: plan.targetURL
        ))
    }

    func testReplaceCarriesExplicitPolicy() throws {
        let source = try makeFile("movie.mp4")
        let existing = try makeFile("movie.srt")
        let planner = BatchOutputPlanner(conflictPolicy: .replace)
        let plan = try planner.plan(source: source, sourceRoot: nil)
        XCTAssertEqual(plan.targetURL, existing.standardizedFileURL)
        XCTAssertEqual(plan.conflict, .replaced(existingURL: existing.standardizedFileURL))
    }

    func testNoConflictWhenTargetAbsent() throws {
        let source = try makeFile("movie.mp4")
        let plan = try BatchOutputPlanner().plan(source: source, sourceRoot: nil)
        XCTAssertEqual(plan.conflict, .none)
    }

    // MARK: - 同批碰撞

    func testDuplicateTargetInBatchFailClosed() throws {
        // 公共根 + 两个独立文件（不同目录）同 basename → 目标都落在 root/movie.srt → 碰撞。
        let root = tempDir.appendingPathComponent("out", isDirectory: true)
        try fileManager.createDirectory(at: root, withIntermediateDirectories: true)
        let dirA = tempDir.appendingPathComponent("a", isDirectory: true)
        let dirB = tempDir.appendingPathComponent("b", isDirectory: true)
        try fileManager.createDirectory(at: dirA, withIntermediateDirectories: true)
        try fileManager.createDirectory(at: dirB, withIntermediateDirectories: true)
        let sourceA = try makeFile("movie.mp4", in: dirA)
        let sourceB = try makeFile("movie.mp4", in: dirB)
        let planner = BatchOutputPlanner(outputRoot: root)
        XCTAssertThrowsError(try planner.planAll(sources: [sourceA, sourceB], sourceRoot: nil)) { error in
            guard case BatchOutputPlannerError.duplicateTarget = error else {
                return XCTFail("同批目标碰撞应 fail-closed，实际 \(error)")
            }
        }
    }

    func testCaseVariantTargetCollisionDetected() throws {
        // 大小写不敏感卷（APFS 默认）：Movie.srt 与 movie.srt 是同一文件 → 折叠碰撞。
        let root = tempDir.appendingPathComponent("out", isDirectory: true)
        try fileManager.createDirectory(at: root, withIntermediateDirectories: true)
        let dirA = tempDir.appendingPathComponent("a", isDirectory: true)
        let dirB = tempDir.appendingPathComponent("b", isDirectory: true)
        try fileManager.createDirectory(at: dirA, withIntermediateDirectories: true)
        try fileManager.createDirectory(at: dirB, withIntermediateDirectories: true)
        let sourceA = try makeFile("Movie.mp4", in: dirA)
        let sourceB = try makeFile("movie.mp4", in: dirB)
        let planner = BatchOutputPlanner(outputRoot: root)
        XCTAssertThrowsError(try planner.planAll(sources: [sourceA, sourceB], sourceRoot: nil)) { error in
            guard case BatchOutputPlannerError.duplicateTarget = error else {
                return XCTFail("大小写变体碰撞应 fail-closed，实际 \(error)")
            }
        }
    }

    func testUnwritableTargetDirectoryFailClosed() throws {
        let lockedDir = tempDir.appendingPathComponent("locked", isDirectory: true)
        try fileManager.createDirectory(at: lockedDir, withIntermediateDirectories: true)
        // 先写入源文件（目录可写时），再锁定目录。
        let sourceInLocked = lockedDir.appendingPathComponent("movie.mp4")
        try Data("x".utf8).write(to: sourceInLocked)
        try fileManager.setAttributes([.posixPermissions: 0o500], ofItemAtPath: lockedDir.path)
        defer { try? fileManager.setAttributes([.posixPermissions: 0o755], ofItemAtPath: lockedDir.path) }
        // sidecar：目标 locked/movie.srt 目录不可写 → unwritableDirectory。
        XCTAssertThrowsError(try BatchOutputPlanner().plan(source: sourceInLocked, sourceRoot: nil)) { error in
            guard case BatchOutputPlannerError.unwritableDirectory = error else {
                return XCTFail("不可写目标目录应 fail-closed，实际 \(error)")
            }
        }
    }
}

// MARK: - 原子写入（08104）

final class AtomicSrtWriterTests: XCTestCase {

    private var tempDir: URL!
    private let fileManager = FileManager.default

    override func setUpWithError() throws {
        tempDir = fileManager.temporaryDirectory
            .appendingPathComponent("sublift-writer-\(UUID().uuidString)", isDirectory: true)
        try fileManager.createDirectory(at: tempDir, withIntermediateDirectories: true)
    }

    override func tearDownWithError() throws {
        try? fileManager.setAttributes([.posixPermissions: 0o755], ofItemAtPath: tempDir.path)
        try fileManager.removeItem(at: tempDir)
    }

    func testWriteCreatesSRTContent() throws {
        let target = tempDir.appendingPathComponent("out.srt")
        try AtomicSrtWriter.write("1\n00:00:00,000 --> 00:00:01,000\nHello\n", to: target)
        let content = try String(contentsOf: target, encoding: .utf8)
        XCTAssertEqual(content, "1\n00:00:00,000 --> 00:00:01,000\nHello\n")
    }

    func testWriteReplacesExistingAtomically() throws {
        let target = tempDir.appendingPathComponent("out.srt")
        try Data("old content".utf8).write(to: target)
        try AtomicSrtWriter.write("new content", to: target)
        XCTAssertEqual(try String(contentsOf: target, encoding: .utf8), "new content")
    }

    func testWriteFailureKeepsExistingBytesIntact() throws {
        let lockedDir = tempDir.appendingPathComponent("locked", isDirectory: true)
        try fileManager.createDirectory(at: lockedDir, withIntermediateDirectories: true)
        let lockedTarget = lockedDir.appendingPathComponent("out.srt")
        // 1. 目录可写时写入 original。
        let original = "original bytes"
        try Data(original.utf8).write(to: lockedTarget)
        // 2. 锁定目录（无写权限）。
        try fileManager.setAttributes([.posixPermissions: 0o500], ofItemAtPath: lockedDir.path)
        defer { try? fileManager.setAttributes([.posixPermissions: 0o755], ofItemAtPath: lockedDir.path) }
        // 3. 尝试替换 → 失败；旧文件字节不变、无临时文件残留。
        XCTAssertThrowsError(try AtomicSrtWriter.write("replacement", to: lockedTarget))
        XCTAssertEqual(try String(contentsOf: lockedTarget, encoding: .utf8), original, "失败时旧文件字节必须不变")
        let leftovers = try fileManager.contentsOfDirectory(atPath: lockedDir.path)
            .filter { $0.contains(".tmp-") }
        XCTAssertTrue(leftovers.isEmpty, "临时文件必须被清理：\(leftovers)")
    }

    func testWriteToUnwritableDirectoryThrows() throws {
        let lockedDir = tempDir.appendingPathComponent("locked", isDirectory: true)
        try fileManager.createDirectory(at: lockedDir, withIntermediateDirectories: true)
        try fileManager.setAttributes([.posixPermissions: 0o500], ofItemAtPath: lockedDir.path)
        defer { try? fileManager.setAttributes([.posixPermissions: 0o755], ofItemAtPath: lockedDir.path) }
        XCTAssertThrowsError(try AtomicSrtWriter.write("x", to: lockedDir.appendingPathComponent("new.srt")))
    }
}
