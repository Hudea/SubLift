# SubLift 进度导航

## 当前状态

- **最后更新：** 2026-08-17
- **当前 Phase：** Phase 12 跨平台 Web UI 与 C++ 原生服务（apps/web-ui 分支）。二轮审核认定 12505 容器化声明与事实不符（模型下载 URL 全 404 且被吞错、Docker 分支从未对真实容器执行、E2E 路径结构性错配），已修复代码（真实 ModelScope 模型源 + SHA256 硬失败、Docker 分支路径映射 + paddle 断言 + golden 比对）并回退 12505 为 in_progress——容器内阶段待有 Docker 环境补证。12506 二轮审核修复（服务端安全收尾、前端状态闭环、验收去假绿）已完成并实测全绿。
- **进度真源：** `phases.json → detail_file`；本文件仅作会话导航。

## 当前计划

- [Phase 12 缺陷修复与端到端闭环计划](docs/plans/features/12501-12505.md)：12501–12504 已验证结项；12505 经二轮审核回退为 in_progress（容器阶段待 Docker 环境实证）。
- [Phase 12 架构设计计划](docs/plans/architecture/phase12-cross-platform-web-ui.md)：C++ Native Web Server + HTTP 206 视频流 + SSE 实时打轴 + Web Workbench 单视频工作台 + 批量任务中心（已完成）。

## 近期完成

- [x] 12508：媒体处理工作区目录显式引导、动态配置与目录级 Cache 体系——未配置时自动展示极简引导屏『请指定视频所在的文件夹路径：』，输入校验合法后自动就近创建 `<media_dir>/.sublift_cache`（`remux/` 与 `frames/` 子目录）并直接进入工作台；已配置直接进入工作台并在 Navbar 呈现 `📁 <dir> (<count> 视频)` 胶囊且支持点击弹窗随时切换；指纹反查与媒体沙箱将 `workspace.media_dir` 列为最高优先级第一搜索根域，拖入工作区内任何视频 100% 毫秒级命中直达提取，视频转封装缓存优先落盘至 `.sublift_cache/remux`。测试：server_test 新增 WorkspaceManager 与 API 2 套用例（Catch2 [server] 243 断言全绿）、Vitest 新增 `system.test.ts`（23/23 全绿）、E2E 新增 `TC-WKS-01/02`（37/37 全绿）、verify-standard 12/12 门禁 100% 全绿。
- [x] 12507：浏览器零拷贝预览与本机指纹反查——拖入/点选视频经 `URL.createObjectURL` 立即本地预览（零上传零拷贝），前端计算 `文件名+大小+首尾 4KB 字节` 指纹经新增 `POST /api/video/resolve` 让服务端在受控目录（沙箱根优先，默认仅用户媒体目录，限深限时、跳符号链接、命中仍须过媒体白名单）定位原文件，命中即零拷贝提取，未命中经信息横幅 + 左侧「服务端路径」输入回退；批量导入反查命中自动入队。实测：活体指纹 200/篡改与 .ts 均 404、路径立即可流式播放；server_test 208 断言、E2E 35/35（新增 TC-RSV）、Vitest 18/18（新增 fingerprint）、ctest 186/186、verify-standard 12/12 全绿。
- [x] 12506：二轮审核修复——`.ts` 白名单移除与沙箱 fail-closed、CORS 默认关/opt-in（`SUBLIFT_CORS_ORIGIN`/`--cors-origin`）、region_box [0,1] 归一化与 clamp UB 修复、Workbench Failed/Cancelled 终态与错误横幅、cancelled SSE 独立回调、批量 catch 保留 cancelled 终态与迟到回调防御、拖拽死路径诚实提示、E2E TC-JOB-07 真实 Paddle 提取 + golden SRT 比对、verify 脚本去假绿、前端严格构建增量缓存假绿修复（删 tsbuildinfo 后真实通过）。实测 [server] 180 断言、E2E 33/33、Vitest 15/15、verify-standard 12/12 全绿。
- [~] 12505（回退为 in_progress）：Dockerfile 模型源修复为 RapidOCR ModelScope v3.9.2 官方 ONNX 产物 + SHA256 硬失败校验；compose 移除空 `./models` 遮蔽挂载；verify-web-server.sh Docker 分支补齐媒体目录挂载/路径映射/`paddle.available=true` 断言/golden 比对。本地降级验收全链路通过；**容器内阶段尚未在真实 Docker 环境执行，待补证后才可置 done**。
- [x] 12504：前端测试工程化接入 (Vitest) 与 E2E 深度断言加固（`package.json` 配置标准 `npm test`、`coordinate_mapper`/`transcript_sync`/`batch` 3 套 13 组测试标准化迁移为 Vitest、重构 `test_server_e2e.py` 严格校验 SRT Exact Match / 排队流转 / 安全沙箱 32/32 全绿、挂载入 `verify-standard.sh`）。
- [x] 12503：前端批量调度器死锁修复、单并发竞态加固与状态闭环（`batch.ts` 重构 `processNext` 消除 `createJob` 异常时的重入死锁、`cancelTask` 同步解耦、`client.ts` 监听 `cancelled` 事件与 `done` 容错、`workbench.ts` 与 `LiveTranscript.vue` 增加 Processing 期间 `isLocked` 保护与 disabled 状态反馈）。
- [x] 12502：C++ 服务端安全沙箱、并发 UAF 消除与防死锁加固（`path_sandbox.hpp` 严格防御 LFI / 穿越 / 空字节注入、`job_manager.cpp` 消除 `active_bridges_` 析构 UAF 并加固 Join、SSE 补齐 `id:` 契约与 `fps` 字段）。
- [x] 12501：流媒体格式兼容性与智能 Remux 极速转封装（`remux_to_faststart_mp4` 极速转封装与原子 rename、`/api/video/stream` 智能分流代理、`useVideoPlayer.ts` 状态重置与 MediaError 友好中文诊断）。
- [x] 12500：完成 Phase 12 全面审计、根因定界与修复规划，登记 12501–12505 修复计划与 HURDLES 记录。
- [x] 12401：Linux 容器化与端到端自动化验收（首期基座）（`Dockerfile` 三阶段多架构构建、`docker-compose.yml` 编排、non-root sublift 用户安全加固、`sublift_server` 环境变量支持、`verify-web-server.sh` 自动化总控回归套件），6 阶段 E2E 验证全绿，标准门禁 10/10 全绿。
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
