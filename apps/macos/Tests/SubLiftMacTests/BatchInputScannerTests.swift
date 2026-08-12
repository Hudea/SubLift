import XCTest
@testable import SubLiftMac

/// 08103：批量输入扫描（真实临时目录）。
final class BatchInputScannerTests: XCTestCase {

    private var tempDir: URL!
    private let fileManager = FileManager.default

    override func setUpWithError() throws {
        tempDir = fileManager.temporaryDirectory
            .appendingPathComponent("sublift-scanner-\(UUID().uuidString)", isDirectory: true)
        try fileManager.createDirectory(at: tempDir, withIntermediateDirectories: true)
    }

    override func tearDownWithError() throws {
        // 恢复权限（权限测试可能留下 0o000 子项）后再删除；目录需可执行位。
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

    private func makeScanner(existing: [URL] = [], ffmpeg: Bool = true) -> BatchInputScanner {
        BatchInputScanner(
            policy: VideoImportPolicy(ffmpegAvailable: { ffmpeg }),
            existingURLs: Set(existing)
        )
    }

    // MARK: - 直接文件输入

    func testDirectVideoFilesAccepted() throws {
        let a = try makeFile("a.mp4")
        let b = try makeFile("b.mov")
        let summary = makeScanner().scan(inputs: [a, b], recursive: false)
        XCTAssertEqual(summary.accepted.map(\.lastPathComponent).sorted(), ["a.mp4", "b.mov"])
        XCTAssertTrue(summary.rejected.isEmpty)
    }

    func testUnsupportedExtensionRejected() throws {
        let txt = try makeFile("notes.txt")
        let mp4 = try makeFile("ok.mp4")
        let summary = makeScanner().scan(inputs: [txt, mp4], recursive: false)
        XCTAssertEqual(summary.accepted, [mp4.standardizedFileURL])
        XCTAssertEqual(summary.rejected.count, 1)
        XCTAssertEqual(summary.rejected[0].reason, .unsupportedFormat("txt"))
    }

    func testMkvRequiresFfmpegWhenUnavailable() throws {
        let mkv = try makeFile("clip.mkv")
        // ffmpeg 可用：MKV 接受。
        let withFfmpeg = makeScanner(ffmpeg: true).scan(inputs: [mkv], recursive: false)
        XCTAssertEqual(withFfmpeg.accepted, [mkv.standardizedFileURL])
        // ffmpeg 缺失：结构化拒绝。
        let withoutFfmpeg = makeScanner(ffmpeg: false).scan(inputs: [mkv], recursive: false)
        XCTAssertTrue(withoutFfmpeg.accepted.isEmpty)
        XCTAssertEqual(withoutFfmpeg.rejected.first?.reason, .mkvRequiresFfmpeg)
    }

    func testUppercaseExtensionAccepted() throws {
        let upper = try makeFile("CLIP.MP4")
        let mixed = try makeFile("clip.Mov")
        let summary = makeScanner().scan(inputs: [upper, mixed], recursive: false)
        XCTAssertEqual(summary.accepted.count, 2)
    }

    func testDuplicateWithinBatchDeduplicated() throws {
        let a = try makeFile("a.mp4")
        let summary = makeScanner().scan(inputs: [a, a], recursive: false)
        XCTAssertEqual(summary.accepted.count, 1)
        XCTAssertEqual(summary.rejected.count, 1)
        XCTAssertEqual(summary.rejected.first?.reason, .duplicate)
    }

    func testDuplicateAgainstExistingQueueRejected() throws {
        let a = try makeFile("a.mp4")
        let summary = makeScanner(existing: [a]).scan(inputs: [a], recursive: false)
        XCTAssertTrue(summary.accepted.isEmpty)
        XCTAssertEqual(summary.rejected.first?.reason, .duplicate)
    }

    func testCaseVariantSameFileDeduplicated() throws {
        // APFS 默认大小写不敏感：.MP4 与 .mp4 是同一物理文件（inode 去重）。
        let a = try makeFile("clip.mp4")
        let upper = tempDir.appendingPathComponent("clip.MP4")
        // 同文件不同拼写：直接构造 URL 指向同一路径（大小写不敏感卷上同一文件）。
        let summary = makeScanner().scan(inputs: [a, upper], recursive: false)
        XCTAssertEqual(summary.accepted.count, 1)
        XCTAssertTrue(summary.rejected.contains { $0.reason == .duplicate })
    }

    func testUnreadableFileRejected() throws {
        let locked = try makeFile("locked.mp4")
        try fileManager.setAttributes([.posixPermissions: 0o000], ofItemAtPath: locked.path)
        defer { try? fileManager.setAttributes([.posixPermissions: 0o644], ofItemAtPath: locked.path) }
        let summary = makeScanner().scan(inputs: [locked], recursive: false)
        XCTAssertTrue(summary.accepted.isEmpty)
        XCTAssertEqual(summary.rejected.first?.reason, .unreadable)
    }

    func testUnreadableDirectoryRejectedNotEmpty() throws {
        // 无权限目录：枚举失败 → unreadable（不是 emptyDirectory）。
        let lockedDir = tempDir.appendingPathComponent("locked", isDirectory: true)
        try fileManager.createDirectory(at: lockedDir, withIntermediateDirectories: true)
        try makeFile("inside.mp4", in: lockedDir)
        try fileManager.setAttributes([.posixPermissions: 0o000], ofItemAtPath: lockedDir.path)
        defer { try? fileManager.setAttributes([.posixPermissions: 0o755], ofItemAtPath: lockedDir.path) }
        let summary = makeScanner().scan(inputs: [lockedDir], recursive: false)
        XCTAssertTrue(summary.accepted.isEmpty)
        XCTAssertEqual(summary.rejected.first?.reason, .unreadable)
    }

    func testNonexistentInputRejectedAsUnreadable() throws {
        let missing = tempDir.appendingPathComponent("missing.mp4")
        let summary = makeScanner().scan(inputs: [missing], recursive: false)
        XCTAssertTrue(summary.accepted.isEmpty)
        XCTAssertEqual(summary.rejected.first?.reason, .unreadable)
    }

    // MARK: - 目录输入与递归

    func testDirectoryNonRecursiveByDefault() throws {
        try makeFile("root.mp4")
        try makeFile("sub/inner.mp4", in: tempDir.appendingPathComponent("sub", isDirectory: true))
        let summary = makeScanner().scan(inputs: [tempDir], recursive: false)
        XCTAssertEqual(summary.accepted.map(\.lastPathComponent), ["root.mp4"], "默认不递归")
    }

    func testRecursiveEnumerationStableOrder() throws {
        try makeFile("b.mp4")
        try makeFile("a.mp4")
        try makeFile("sub/c.mp4")
        try makeFile("sub/deep/d.mp4")
        let summary = makeScanner().scan(inputs: [tempDir], recursive: true)
        // 相对路径稳定排序：a.mp4, b.mp4, sub/c.mp4, sub/deep/d.mp4
        let paths = summary.accepted.map { $0.path.replacingOccurrences(of: tempDir.path + "/", with: "") }
        XCTAssertEqual(paths, ["a.mp4", "b.mp4", "sub/c.mp4", "sub/deep/d.mp4"])
    }

    func testHiddenAndPackageAndSymlinkExcluded() throws {
        try makeFile(".hidden.mp4")
        try makeFile("visible.mp4")
        // package 目录：LaunchServices 注册的 package 扩展名目录（如 .pkg）isPackage=true。
        let pkg = tempDir.appendingPathComponent("bundle.pkg", isDirectory: true)
        try fileManager.createDirectory(at: pkg, withIntermediateDirectories: true)
        try makeFile("inside.mp4", in: pkg)
        // symlink 指向真实文件
        let real = try makeFile("real.mp4")
        let link = tempDir.appendingPathComponent("link.mp4")
        try fileManager.createSymbolicLink(at: link, withDestinationURL: real)

        let summary = makeScanner().scan(inputs: [tempDir], recursive: true)
        let accepted = summary.accepted.map(\.lastPathComponent)
        XCTAssertFalse(accepted.contains(".hidden.mp4"), "隐藏文件不进入扫描")
        XCTAssertFalse(accepted.contains("inside.mp4"), "package 内容不进入扫描")
        XCTAssertFalse(accepted.contains("link.mp4"), "symlink 不进入扫描")
        XCTAssertTrue(accepted.contains("visible.mp4"))
        XCTAssertTrue(accepted.contains("real.mp4"))
        // 08309/08410：静默排除计入 skipped（隐藏文件 + symlink 各 1）。
        XCTAssertEqual(summary.skipped, 2, "隐藏文件与 symlink 计入跳过")
    }

    func testEmptyDirectoryRejected() throws {
        let empty = tempDir.appendingPathComponent("empty", isDirectory: true)
        try fileManager.createDirectory(at: empty, withIntermediateDirectories: true)
        let summary = makeScanner().scan(inputs: [empty], recursive: false)
        XCTAssertTrue(summary.accepted.isEmpty)
        XCTAssertEqual(summary.rejected.first?.reason, .emptyDirectory)
    }

    // MARK: - 有界性

    func testThousandItemsScanBounded() throws {
        let big = tempDir.appendingPathComponent("big", isDirectory: true)
        try fileManager.createDirectory(at: big, withIntermediateDirectories: true)
        for i in 0..<1000 {
            try makeFile(String(format: "f%04d.mp4", i), in: big)
        }
        let start = Date()
        let summary = makeScanner().scan(inputs: [big], recursive: false)
        let elapsed = Date().timeIntervalSince(start)
        XCTAssertEqual(summary.accepted.count, 1000)
        XCTAssertTrue(summary.rejected.isEmpty)
        XCTAssertLessThan(elapsed, 10, "1000 项扫描应有界（实际 \(elapsed)s）")
    }

    // MARK: - 混合输入摘要

    func testMixedInputSummary() throws {
        let good = try makeFile("good.mp4")
        let txt = try makeFile("bad.txt")
        let summary = makeScanner().scan(inputs: [good, txt, tempDir], recursive: true)
        // good.mp4 直接输入 + 目录内同文件：去重后 accepted 1；bad.txt 拒绝。
        XCTAssertEqual(summary.accepted.count, 1)
        XCTAssertEqual(summary.accepted.first?.lastPathComponent, "good.mp4")
        XCTAssertTrue(summary.rejected.contains { $0.reason == .unsupportedFormat("txt") })
        XCTAssertTrue(summary.rejected.contains { $0.reason == .duplicate }, "目录内重叠文件应记为重复")
    }
}
