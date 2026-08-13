import XCTest
@testable import SubLiftMac

// MARK: - Task Center 展示纯逻辑（08308）

final class TaskCenterPresentationTests: XCTestCase {

    private func makeTask(_ name: String = "movie.mp4", status: BatchTaskStatus = .waiting) -> BatchTask {
        var task = BatchTask.make(
            sourceURL: URL(fileURLWithPath: "/tmp/\(name)"),
            engine: .vision,
            quality: .fast,
            developerMode: false
        )
        advance(&task, to: status)
        return task
    }

    private func advance(_ task: inout BatchTask, to target: BatchTaskStatus) {
        switch target {
        case .preparing: _ = task.transition(to: .preparing)
        case .extracting:
            _ = task.transition(to: .preparing)
            _ = task.transition(to: .extracting)
        case .exporting:
            _ = task.transition(to: .preparing)
            _ = task.transition(to: .extracting)
            _ = task.transition(to: .exporting)
        case .completed:
            _ = task.transition(to: .preparing)
            _ = task.transition(to: .extracting)
            _ = task.transition(to: .exporting)
            _ = task.transition(to: .completed)
        case .failed:
            _ = task.transition(to: .preparing)
            _ = task.transition(to: .failed)
        case .cancelled:
            _ = task.transition(to: .cancelled)
        case .interrupted:
            _ = task.transition(to: .preparing)
            _ = task.transition(to: .interrupted)
        case .skipped:
            _ = task.transition(to: .skipped)
        case .waiting:
            break
        }
    }

    // MARK: - 汇总

    func testSummaryCountsRealStates() {
        var queue = BatchQueueState.empty
        queue.tasks = [
            makeTask("a.mp4", status: .waiting),
            makeTask("b.mp4", status: .waiting),
            makeTask("c.mp4", status: .completed),
            makeTask("d.mp4", status: .completed),
            makeTask("e.mp4", status: .failed),
            makeTask("f.mp4", status: .cancelled),
            makeTask("g.mp4", status: .skipped),
            makeTask("h.mp4", status: .extracting),
        ]
        let summary = TaskCenterPresentation.summary(for: queue)
        XCTAssertEqual(summary.total, 8)
        XCTAssertEqual(summary.waiting, 2)
        XCTAssertEqual(summary.active, 1)
        XCTAssertEqual(summary.completed, 2)
        XCTAssertEqual(summary.failed, 1)
        XCTAssertEqual(summary.cancelled, 1)
        XCTAssertEqual(summary.skipped, 1)
    }

    func testSummaryEmptyQueue() {
        let summary = TaskCenterPresentation.summary(for: .empty)
        XCTAssertEqual(summary.total, 0)
        XCTAssertEqual(summary.completed, 0)
    }

    // MARK: - 状态投影（真实字段）

    func testStatusDisplayNameMapsAllCases() {
        XCTAssertEqual(TaskCenterPresentation.statusDisplayName(.waiting), "等待中")
        XCTAssertEqual(TaskCenterPresentation.statusDisplayName(.preparing), "准备中")
        XCTAssertEqual(TaskCenterPresentation.statusDisplayName(.extracting), "提取中")
        XCTAssertEqual(TaskCenterPresentation.statusDisplayName(.exporting), "导出中")
        XCTAssertEqual(TaskCenterPresentation.statusDisplayName(.completed), "已完成")
        XCTAssertEqual(TaskCenterPresentation.statusDisplayName(.failed), "失败")
        XCTAssertEqual(TaskCenterPresentation.statusDisplayName(.cancelled), "已取消")
        XCTAssertEqual(TaskCenterPresentation.statusDisplayName(.interrupted), "已中断")
        XCTAssertEqual(TaskCenterPresentation.statusDisplayName(.skipped), "已跳过")
    }

    func testProgressTextUsesRealProgressOnly() {
        var task = makeTask("a.mp4", status: .extracting)
        task.setProgress(0.42)
        XCTAssertEqual(TaskCenterPresentation.progressText(for: task), "42%")
        // 无进度（初始 nil）不编造。
        let noProgress = makeTask("a.mp4", status: .extracting)
        XCTAssertNil(TaskCenterPresentation.progressText(for: noProgress))
        // 终态进度不显示假百分比。
        let done = makeTask("b.mp4", status: .completed)
        XCTAssertNil(TaskCenterPresentation.progressText(for: done))
    }

    func testEngineDisplayNameReusesMapping() {
        let task = makeTask("a.mp4")
        XCTAssertEqual(TaskCenterPresentation.engineDisplayName(for: task), OcrEngineName.vision.displayName)
    }

    func testDurationTextFromResultOnly() {
        // 时长列：仅 completed 有真实 duration 字段（result 无 duration——用 "-" 表示未可知）。
        let waiting = makeTask("a.mp4")
        XCTAssertEqual(TaskCenterPresentation.durationText(for: waiting), "—")
    }

    // MARK: - 详情展示

    func testDetailConfigUsesRealConfiguration() {
        var task = makeTask("a.mp4")
        _ = task.replaceConfiguration(ExtractionConfiguration(engine: .paddle, quality: .fine))
        let detail = TaskCenterPresentation.detailRows(for: task)
        XCTAssertTrue(detail.contains { $0.label == "配置" && $0.value.contains("PaddleOCR") })
        XCTAssertTrue(detail.contains { $0.label == "配置" && $0.value.contains("精细") })
        XCTAssertTrue(detail.contains { $0.label == "Task ID" && $0.value == task.id.uuidString })
    }

    func testDetailShowsFailureMessageWhenPresent() {
        var task = makeTask("a.mp4")
        _ = task.transition(to: .preparing)
        task.recordFailure("worker error: timeout")
        _ = task.transition(to: .failed)
        let detail = TaskCenterPresentation.detailRows(for: task)
        XCTAssertTrue(detail.contains { $0.label == "错误" && $0.value.contains("timeout") })
    }

    func testNoFabricatedFieldsInDetail() {
        let task = makeTask("a.mp4", status: .extracting)
        let detail = TaskCenterPresentation.detailRows(for: task)
        XCTAssertFalse(detail.contains { $0.label.contains("ETA") })
        XCTAssertFalse(detail.contains { $0.label.contains("置信度") })
        XCTAssertFalse(detail.contains { $0.label.contains("下载") })
    }

    func testDetailAfterRequeueHidesOldFailure() {
        // requeue 清除旧失败元数据后，详情不应再展示上一轮的「错误」行。
        var task = makeTask("a.mp4")
        _ = task.transition(to: .preparing)
        task.recordFailure("worker error: timeout")
        _ = task.transition(to: .failed)
        XCTAssertTrue(TaskCenterPresentation.detailRows(for: task).contains { $0.label == "错误" })

        task.requeue()

        let detail = TaskCenterPresentation.detailRows(for: task)
        XCTAssertEqual(task.status, .waiting)
        XCTAssertFalse(detail.contains { $0.label == "错误" }, "重试后详情不应残留旧错误")
        XCTAssertFalse(detail.contains { $0.label == "输出" && $0.value.contains("Result") }, "重试后详情不应残留旧结果")
    }
}

// MARK: - Task Center 命令 availability（08308）

final class TaskCenterCommandTests: XCTestCase {

    private func makeTask(_ name: String, status: BatchTaskStatus = .waiting) -> BatchTask {
        var task = BatchTask.make(
            sourceURL: URL(fileURLWithPath: "/tmp/\(name)"),
            engine: .vision,
            quality: .fast,
            developerMode: false
        )
        if status != .waiting {
            _ = task.transition(to: status == .failed ? .failed : (status == .cancelled ? .cancelled : status))
        }
        return task
    }

    private func queue(_ tasks: [BatchTask], status: BatchQueueStatus = .idle) -> BatchQueueState {
        BatchQueueState(status: status, tasks: tasks, runningTaskID: nil)
    }

    func testQueueLevelAvailability() {
        // 空队列：不可开始。
        XCTAssertFalse(TaskCenterPresentation.canStart(.empty))
        // 有 waiting：可开始。
        let waitingQueue = queue([makeTask("a.mp4")])
        XCTAssertTrue(TaskCenterPresentation.canStart(waitingQueue))
        // running：可暂停/停止，不可开始。
        let runningQueue = queue([makeTask("a.mp4", status: .extracting)], status: .running)
        XCTAssertFalse(TaskCenterPresentation.canStart(runningQueue))
        XCTAssertTrue(TaskCenterPresentation.canPauseAfterCurrent(runningQueue))
        XCTAssertTrue(TaskCenterPresentation.canStop(runningQueue))
        XCTAssertFalse(TaskCenterPresentation.canResume(runningQueue))
        // paused：可恢复。
        let pausedQueue = queue([makeTask("a.mp4")], status: .paused)
        XCTAssertTrue(TaskCenterPresentation.canResume(pausedQueue))
        XCTAssertFalse(TaskCenterPresentation.canStop(pausedQueue))
    }

    func testTaskLevelAvailabilityReusesCommandAvailability() {
        // 任务级命令复用 BatchTaskCommandAvailability 唯一真源。
        XCTAssertTrue(BatchTaskCommandAvailability.canCancel(.extracting))
        XCTAssertTrue(BatchTaskCommandAvailability.canRetry(.failed))
        XCTAssertFalse(BatchTaskCommandAvailability.canRetry(.completed))
        XCTAssertTrue(BatchTaskCommandAvailability.canRemove(.waiting))
        XCTAssertFalse(BatchTaskCommandAvailability.canRemove(.extracting))
    }

    // MARK: - 08511 空画布

    func testShouldShowEmptyCanvasWhenNoTasks() {
        XCTAssertTrue(TaskCenterPresentation.shouldShowEmptyCanvas(for: .empty))
    }

    func testShouldNotShowEmptyCanvasWhenTasksExist() {
        let q = queue([makeTask("a.mp4")], status: .idle)
        XCTAssertFalse(TaskCenterPresentation.shouldShowEmptyCanvas(for: q))
    }

    func testShouldNotShowEmptyCanvasWhenFilteredEmpty() {
        // 筛选结果为空 ≠ 空态——队列有任务就不展示 drop zone。
        let q = queue([makeTask("a.mp4", status: .completed)])
        XCTAssertFalse(TaskCenterPresentation.shouldShowEmptyCanvas(for: q))
    }

    // MARK: - 08511 位置列与四档列宽

    func testVisibleColumnsNarrowShowsFileStatusProgress() {
        let cols = TaskCenterPresentation.visibleColumns(forWidth: 800)
        XCTAssertEqual(cols, [.file, .status, .progress])
    }

    func testVisibleColumnsMediumAddsOutput() {
        let cols = TaskCenterPresentation.visibleColumns(forWidth: 1000)
        XCTAssertTrue(cols.contains(.output))
        XCTAssertFalse(cols.contains(.location))
    }

    func testVisibleColumnsWideAddsLocation() {
        let cols = TaskCenterPresentation.visibleColumns(forWidth: 1150)
        XCTAssertEqual(cols[1], .location, "位置应插在文件与状态之间")
    }

    func testVisibleColumnsFullAddsEngineDurationAdded() {
        let cols = TaskCenterPresentation.visibleColumns(forWidth: 1300)
        XCTAssertTrue(cols.contains(.engine))
        XCTAssertTrue(cols.contains(.duration))
        XCTAssertTrue(cols.contains(.added))
    }

    func testLocationDisplayWithImportRoot() {
        var task = makeTask("movie.mp4")
        let root = URL(fileURLWithPath: "/tmp/Videos/Zootopia")
        task = BatchTask.make(
            sourceURL: URL(fileURLWithPath: "/tmp/Videos/Zootopia/movie.mp4"),
            engine: .vision, quality: .fast, developerMode: false,
            importRootURL: root
        )
        let display = TaskCenterPresentation.locationDisplay(for: task)
        XCTAssertEqual(display, "Zootopia/")
    }

    func testLocationDisplayWithNilImportRoot() {
        let task = makeTask("movie.mp4")
        // importRootURL 默认为 nil
        let display = TaskCenterPresentation.locationDisplay(for: task)
        XCTAssertEqual(display, "tmp")
    }

    func testMatchesSearchMatchesFilename() {
        let task = makeTask("Zootopia.mp4")
        XCTAssertTrue(TaskCenterPresentation.matchesSearch(task, query: "zoo"))
        XCTAssertFalse(TaskCenterPresentation.matchesSearch(task, query: "nonexistent"))
    }

    func testMatchesSearchMatchesFullPath() {
        let task = BatchTask.make(
            sourceURL: URL(fileURLWithPath: "/tmp/Movies/Zootopia/movie.mp4"),
            engine: .vision, quality: .fast, developerMode: false
        )
        XCTAssertTrue(TaskCenterPresentation.matchesSearch(task, query: "Movies"))
    }
}
