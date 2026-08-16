# SubLift 进度导航

## 当前状态

- **最后更新：** 2026-08-16
- **当前 Phase：** Phase 12 跨平台 Web UI 与 C++ 原生服务（apps/web-ui 分支）。12001（设计基线）、12101（C++ Web Server 与视频流）、12102（SSE 实时流与 Pipeline 调度）、12201（Web 单视频工作台基础 UI）、12202（Canvas ROI 选区交互与坐标映射）、12203（双向音画联动与 SRT 导出）与 12301（批量任务中心与串行调度引擎）已完成，下一步推进 12401（Linux 容器化与端到端自动化验收）。Phase 8、Phase 7、Phase 10 已收口。
- **进度真源：** `phases.json → detail_file`；本文件仅作会话导航。

## 当前计划

- [Phase 12 架构设计计划](docs/plans/architecture/phase12-cross-platform-web-ui.md)：C++ Native Web Server + HTTP 206 视频流 + SSE 实时打轴 + Web Workbench 单视频工作台。

## 近期完成

- [x] 12301：实现批量任务中心 (Task Center UI & Queue)（`types/batch.ts` 与 `stores/batch.ts` 单并发串行调度引擎、调度防重入互斥锁与 SSE 异常恢复、`BatchStatsCards.vue` 5 状态筛选面板、`BatchDropZone.vue` 多选拖拽与多行绝对路径录入、`BatchTaskRow.vue` 呼吸指示灯与操作栏、`Navbar.vue` Tab 切换与未完成角标、`TaskCenterView.vue` 与错峰批量 SRT 导出），通过 `batch.test.ts` 异步测试与双审计，Vue 3 严格构建 0 错误，E2E 29 组全绿。
- [x] 12203：实现双向音画联动、字幕行内编辑与 SRT 导出（`subtitle_search.ts` 10Hz 时钟 $O(1)$ 缓存命中与 $O(\log N)$ 二分快速定位、`srt_formatter.ts` 标准 UTF-8 导出与弹性时码解析、`LiveTranscript.vue` 实时推流吸附与呼吸高亮平滑滚动、双击就地文本/时码编辑与打轴操作），单元测试与 E2E 29 组全绿通过。
- [x] 12202：实现 Canvas ROI 选区交互与坐标映射（`coordinate_mapper.ts` 消除 Letterbox 黑边、`RoiOverlay.vue` 8 控制手柄缩放/平移/画新框、浮动真实尺寸 Tooltip 胶囊、Retina 高清屏适配、与 `workbenchStore.regionBox` 双向响应），单元测试与 E2E 29 组全绿通过。
- [x] 12201：构建 Web 单视频工作台基础 UI（`apps/web/` Vue 3 + TS + Vite SPA、Apple/Notion 磨砂深色设计系统、HTML5 播放器与 10Hz 时码同步 `HH:MM:SS.mmm`、4 态状态机与 DropZone/Navbar/SidebarControls/LiveTranscript 组件、`sublift_server --static-dir` 静态托管），E2E 29 组测试全绿通过。
- [x] 12102：实现 SSE 实时事件流与 C++ Pipeline 任务调度（`JobManager` 任务状态机与 UUID v4、`BridgeHandler` 进程内流水线驱动、`GET /api/jobs/{id}/events` SSE 流式推送、`POST /api/jobs/{id}/cancel` 协作取消、`GET /api/jobs/{id}/export` SRT 导出下载），Catch2 147 组断言与 Python E2E 28 组测试全绿通过。
- [x] 12101：C++ Native Web Server 基座与 HTTP 206 视频流实现（`sublift_server` 单二进制、vendored `cpp-httplib`、`/api/system/info` 运行时与引擎探活、`/api/video/stream` 206 Partial Content 分片流、`/api/video/frame` 快速 JPEG 截帧），Catch2 60 组断言与 curl 完整验证通过。
- [x] 12001：Phase 12 架构设计与实施基线冻结（C++ 零 Python 依赖/零 IPC 开销、HTTP 206 视频流、SSE 实时事件流与 Web 单视频工作台交互规范），通过 schema 与 ./init.sh 门禁。
- [x] 08511 收口：重拍 T01–T07（七张 MD5 不同）；修虚线 overlay、四档 Table 变体、beginImport 异步 present、单选隐藏底栏、WAITING/SELECT/OUTPUT_ROOT fixture。完整 swift test 371 XCTest + 140 Swift Testing。
- [x] 08511-C5：Inspector 卡片——TaskInspectorModel + inspectorModel、TaskDetailView 重写为卡片布局（页眉含删除/上移/下移/取消/重试）、输出位置 Toolbar 菜单 + NSOpenPanel、labelsHidden Picker。commit `2a14e87`。
- [x] 08511-C4：位置列——TaskTableColumn 枚举 + visibleColumns 四档列宽、locationDisplay/locationFullPath/locationTooltip/matchesSearch、搜索扩展到位置+路径、位置不可见时折进文件单元格。commit `9eb98cc`。
- [x] 08511-C3：空画布——shouldShowEmptyCanvas、虚线 drop zone、单一 fileImporter + TaskCenterImportKind、NSOpenPanel 文件夹选择、isDropTargeted 高亮、筛选栏空队列禁用、DROP fixture。commit `93343d8`。
- [x] 08511-C2：输出规划可见性——`BatchOutputDestination` 显式 Codable、`previewTarget` 纯路径、`BatchOutputPlanner` 迁移到 `BatchPath.relativePath`、`planOutputsForWaitingTasks` 导入即预览 + 同批碰撞先到先得、`confirmOutputConflictsAndStart` guard planningErrors、`cancelStart` 保留预览 outputURL、`retry` 后 replan、Scheduler `setOutputURLs` 批量 + `setOutputDestination` 持久化、`outputDestination` 三级持久化（State→Snapshot→Repository）、354 XCTest + 140 Swift Testing 全绿。commit `d073ae7`。
- [x] 08511-C1：Domain 基础——`BatchAcceptedItem`、`BatchPath.relativePath`、`BatchTask.importRootURL`（decodeIfPresent 兼容旧 JSON）、Scanner set importRoot、Model importInputs 接线。commit `051269d`。
- [x] 08410-fix：修复 Phase 8 综合审核发现的两个合入前缺口——`BatchTask.requeue()` 现在清除 `failureMessage`/`progress`/`result`（保留 `outputURL`）；Task Center 在 `TaskDetailView` 与 Table contextMenu 接入单任务「取消/重试」按钮，按 `BatchTaskCommandAvailability` 启用；另补 Workspace Toolbar「任务中心」入口按钮（原入口仅在菜单/⌘⇧T，主窗口不可见）。新增 4 个回归测试。本轮另清理了 Swift 6 Sendable 前置 warning（`BatchOutputPlanner` 移除 `FileManager` 存储属性、`OcrEngineName`/`SamplingQuality` 显式 `Sendable`、移除 TaskCenterView 冗余 `_ =`）。完整 Swift 测试 331 XCTest + 140 Swift Testing 全绿，`swift build` 无 warning。
- [x] 08410：Phase 8 综合审核与验收——真实混合目录（支持/无效/重复/嵌套/已有 SRT）、真实串行 IPC 闭环（2 视频 completed + 不存在文件 fail-closed failed + retry 回 waiting + 输出定位）、恢复/资源（100 任务 41.7KB/0.001s、损坏 fail-closed、运行后零残留 Worker）、B01–B10 证据索引、项目门与文档收口（REQUIREMENTS/ARCHITECTURE/macos-gui/phases.json 同步）。
- [x] 08309：Task Center 批量交互——统一导入（文件/文件夹/drop 同一 Scanner + B02 三类别摘要）、搜索/状态筛选投影、waiting 多选删除/重排/显式改配置、输出冲突开始前集中确认（取消零文件触碰）、Finder 定位、B02/B03/B08/B09 截图。
- [x] 08308：原生 Task Center Window——独立 WindowGroup + ⌘⇧T（openWindow 规范路径）、原生 Table 7 列/详情/Toolbar/汇总条、scheduler.onStateChange live 更新、恢复 fail-closed、B01–B10 截图。
- [x] 08001：Phase 8 产品合同、双组合根/串行调度/安全输出/JSON 恢复架构与 08102–08410 验收拆解已冻结；实现尚未开始，证据见 `docs/phases/phase8.json`。
- [x] 10416：Phase 10 当前文档与计划状态已统一到 10415 最终实现；201+140 Swift 测试、标准门 10/10、JSON Schema、依赖/状态、318 个本地链接、diff check 与 init 全部通过，并与 07002 台账迁移共同组成最终纯文档提交。
- [x] 07002：原 Phase 9 的 09001–09006 已无损吸收到 Phase 7 项目辅助架构；历史 ID/evidence 保留，Phase 9 索引与详情路径已释放，未来从 09101 起登记。
- [x] 10415：快速提取设置栏已完成；ExtractionProgressView 下方 52–64pt 紧凑栏（引擎 Popup + 采样分段 + 待生效提示 + 重新提取）、active/final 配置快照生命周期、统一 requestExtraction 与替换确认、Inspector 去重只读化、V09/V09b 截图证据见 `docs/phases/phase10.json`。
- [x] 10414：Phase 10 独立审计修复与真实 UI 验收完成；Mock fail-closed、Inspector 紧凑布局、first-responder Esc、源序号搜索及视觉细节均已修复，177+140 Swift 测试与标准门 10/10 通过，证据见 `docs/phases/phase10.json`。

## 阻塞项 / 风险（如实记录）

- 完整 `swift test` 前必须 `export PYTHONPATH=`（Hermes 会话 PYTHONPATH 污染 .venv 子进程会导致集成测试挂起）。
- mock/vision 引擎对测试视频提取均生成 0 条（Worker 管线行为，未改动）；含条目的 UI 截图使用 DEBUG 注入 fixture。
- 真实 UI 交互（点击/键盘/VoiceOver）受系统"辅助功能访问（事件）"权限限制未执行；以单测 + 代码审核 + 渲染核验覆盖（逐 Feature 在 evidence 记录）。

## 近期决策

- ADR-0037：Phase 8 采用独立 Task Center + 单并发队列；入队配置快照、SRT 安全写入与 JSON 显式恢复，不扩张 Workspace 或 Worker/IPC。
- ADR-0036：项目辅助架构统一归入 Phase 7，保留 090xx 历史 ID 并释放 Phase 9；未来新 Phase 9 从 09101 起登记。
- ADR-0035：Phase 10 采用无永久 Sidebar 的 Native Workbench、Context Inspector 与处理期只读 Transcript。

> **Phase 8 已完成**（08001–08410，2026-08-13 综合验收收口）：F24 批量处理队列已实现（独立 Task Center、统一导入、串行队列、安全 SRT 输出、JSON 恢复与批量交互），全 Feature 已原子提交至 `feat/task-list` 分支并记录 evidence。
