import XCTest
@testable import SubLiftMac

// MARK: - Task Center 交互（08309：多选/确认/Finder/availability）

final class TaskCenterInteractionTests: XCTestCase {

    private func makeTask(_ name: String, status: BatchTaskStatus = .waiting) -> BatchTask {
        var task = BatchTask.make(
            sourceURL: URL(fileURLWithPath: "/tmp/\(name)"),
            engine: .vision,
            quality: .fast,
            developerMode: false
        )
        switch status {
        case .preparing: _ = task.transition(to: .preparing)
        case .extracting:
            _ = task.transition(to: .preparing); _ = task.transition(to: .extracting)
        case .exporting:
            _ = task.transition(to: .preparing); _ = task.transition(to: .extracting)
            _ = task.transition(to: .exporting)
        case .completed:
            _ = task.transition(to: .preparing); _ = task.transition(to: .extracting)
            _ = task.transition(to: .exporting); _ = task.transition(to: .completed)
        case .failed:
            _ = task.transition(to: .preparing); _ = task.transition(to: .failed)
        case .cancelled: _ = task.transition(to: .cancelled)
        case .interrupted:
            _ = task.transition(to: .preparing); _ = task.transition(to: .interrupted)
        case .skipped: _ = task.transition(to: .skipped)
        case .waiting: break
        }
        return task
    }

    // MARK: - 多选删除 availability

    func testBatchRemoveOnlyWaitingTasks() {
        let waiting1 = makeTask("a.mp4")
        let waiting2 = makeTask("b.mp4")
        let running = makeTask("c.mp4", status: .extracting)
        let done = makeTask("d.mp4", status: .completed)

        let removable = TaskCenterInteraction.removableTaskIDs(from: [waiting1, waiting2, running, done])
        XCTAssertEqual(removable.count, 3, "非活动态可删，进行中不可删")
        XCTAssertTrue(removable.contains(waiting1.id))
        XCTAssertTrue(removable.contains(waiting2.id))
        XCTAssertTrue(removable.contains(done.id))
        XCTAssertFalse(removable.contains(running.id))
    }

    // MARK: - 重排（waiting 槽位保留）

    func testMoveUpDownPreservesSlots() {
        let a = makeTask("a.mp4")
        let b = makeTask("b.mp4")
        let done = makeTask("d.mp4", status: .completed)
        var tasks = [a, b, done, makeTask("c.mp4")]

        // c（waiting 槽位 3）上移 → 与 b（waiting 槽位 1）交换。
        TaskCenterInteraction.moveTask(id: tasks[3].id, in: &tasks, direction: .up)
        let names = tasks.map { $0.sourceURL.lastPathComponent }
        XCTAssertEqual(names, ["a.mp4", "c.mp4", "d.mp4", "b.mp4"], "仅 waiting 槽位交换，done 槽位不动")
    }

    // MARK: - 替换确认决策

    func testReplaceDecisionConfirmedStartsWithReplacement() {
        let conflict = TaskCenterInteraction.OutputConflict(taskID: UUID(), outputURL: URL(fileURLWithPath: "/tmp/a.srt"))
        let decision = TaskCenterInteraction.replaceDecision(confirming: true, conflicts: [conflict])
        XCTAssertEqual(decision, .replaceAndStart)
    }

    func testReplaceDecisionCancelledDoesNotStart() {
        let decision = TaskCenterInteraction.replaceDecision(confirming: false, conflicts: [.init(taskID: UUID(), outputURL: URL(fileURLWithPath: "/tmp/a.srt"))])
        XCTAssertEqual(decision, .cancel)
    }

    // MARK: - Finder 定位

    func testFinderRevealExistingFileResolvesURL() throws {
        let tempDir = FileManager.default.temporaryDirectory.appendingPathComponent("tcint-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: tempDir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: tempDir) }
        let file = tempDir.appendingPathComponent("a.mp4")
        try Data("x".utf8).write(to: file)

        let resolved = TaskCenterInteraction.revealCandidate(for: file)
        XCTAssertEqual(resolved, file, "存在文件直接定位")
    }

    func testFinderRevealMissingFileReturnsNil() {
        let missing = URL(fileURLWithPath: "/tmp/definitely-missing-\(UUID().uuidString).mp4")
        XCTAssertNil(TaskCenterInteraction.revealCandidate(for: missing))
    }

    func testImportKindDistinguishesFilesAndFolder() {
        XCTAssertNotEqual(TaskCenterImportKind.files, TaskCenterImportKind.folder)
    }
}

// MARK: - Task Center 无障碍（08309：AX label/value 组合、状态不只依赖颜色）

final class TaskCenterAccessibilityTests: XCTestCase {

    func testStatusAccessibilityCombinesTextAndSymbol() {
        // 状态可读性：AX label 含状态文本（不只颜色）。
        let label = TaskCenterAccessibility.statusLabel(for: .failed)
        XCTAssertEqual(label, "失败")
        let running = TaskCenterAccessibility.statusLabel(for: .extracting)
        XCTAssertEqual(running, "提取中")
    }

    func testProgressAccessibilityReadsRealValue() {
        var task = BatchTask.make(
            sourceURL: URL(fileURLWithPath: "/tmp/a.mp4"),
            engine: .vision, quality: .fast, developerMode: false
        )
        _ = task.transition(to: .preparing)
        _ = task.transition(to: .extracting)
        task.setProgress(0.42)
        XCTAssertEqual(TaskCenterAccessibility.progressLabel(for: task), "进度 42%")
        // 无进度不编造。
        let waiting = BatchTask.make(
            sourceURL: URL(fileURLWithPath: "/tmp/b.mp4"),
            engine: .vision, quality: .fast, developerMode: false
        )
        XCTAssertNil(TaskCenterAccessibility.progressLabel(for: waiting))
    }

    func testSummaryAccessibilityCombinesCounts() {
        let label = TaskCenterAccessibility.summaryLabel(total: 3, waiting: 1, active: 1, completed: 1, failed: 0, cancelled: 0, skipped: 0)
        XCTAssertTrue(label.contains("总数 3"))
        XCTAssertTrue(label.contains("等待 1"))
        XCTAssertTrue(label.contains("进行中 1"))
        XCTAssertTrue(label.contains("完成 1"))
        XCTAssertFalse(label.contains("队列运行中"))
    }

    func testSummaryLabelIncludesQueueStatusWhenProvided() {
        let label = TaskCenterAccessibility.summaryLabel(
            total: 3, waiting: 1, active: 1, completed: 1, failed: 0, cancelled: 0, skipped: 0,
            queueStatus: "队列运行中 · 当前 a.mp4"
        )
        XCTAssertTrue(label.contains("总数 3"))
        XCTAssertTrue(label.contains("队列运行中 · 当前 a.mp4"))
    }

    func testTransportStatusTextForRunningIncludesFilename() {
        var task = BatchTask.make(
            sourceURL: URL(fileURLWithPath: "/tmp/clip.mp4"),
            engine: .vision, quality: .fast, developerMode: false
        )
        _ = task.transition(to: .preparing)
        _ = task.transition(to: .extracting)
        var queue = BatchQueueState.empty
        queue.status = .running
        queue.tasks = [task]
        let statusText = TaskCenterPresentation.transport(for: queue, reason: .none).statusText
        XCTAssertTrue(statusText.contains("队列运行中"))
        XCTAssertTrue(statusText.contains("clip.mp4"))
    }
}
