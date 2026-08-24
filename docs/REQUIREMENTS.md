# SubLift 需求规格

## 1. 项目目标

为桌面用户提供一个**本地、离线、免费**的硬字幕提取工具，把烧录在视频画面中的字幕还原为可编辑字幕。产品以 Native C++ 作为唯一运行时，由 Native CLI、macOS SwiftUI 和 Web/Native Server 提供入口；Vision、PaddleOCR 和 Mock 均通过 Native 边界接入，但 Windows/Linux 桌面 GUI 仍是后续范围。

**当前运行时状态：** Phase 6.0–6.9 已完成 C++ Core 与全引擎 cutover，Phase 13 已完成
Python 产品 CLI、Pipeline、IPC 与 runtime 路由退役并在 `main` 置 `done`。产品只使用 Native
C++ runtime；capability 缺失时必须 fail-closed，回滚到上一版已验收的 Native artifact、
release/tag 或 Git revision，不切换到 Python。Python 仅保留为隔离的可选离线工具与冻结
Oracle。现行契约见 [`docs/cpp/`](cpp/README.md)。

## 2. 用户与使用场景

| 用户类型 | 典型场景 |
|---|---|
| 字幕组 / 翻译者 | 二次校对、翻译已烧录字幕的影视片段 |
| 内容创作者 | 把旧素材中的硬字幕重新结构化，便于再剪辑 |
| 学习者 | 从带字幕的教程视频中导出文本，便于检索与笔记 |
| 开发者 / 技术用户 | 通过 CLI 批量处理视频文件 |

## 3. 功能需求

> 状态说明：❌ 未实现 · ⚠️ 部分实现或仍有限制 · ✅ 已完成

### 3.1 核心提取能力

- **F1 视频输入** ✅：通过 ffmpeg 支持 mp4 / mkv / mov 等主流容器及 H.264 / H.265。
- **F2 帧采样** ✅：按可配置采样率抽帧，默认 5fps；产品 path mode 由 Native
  `FfmpegExtractor` 统一处理，并遵循版本化抽帧 golden 与 Native 契约测试。
- **F3 字幕区域** ✅：CLI 默认裁剪画面下部 30%，支持固定区域；macOS GUI 用 Vision 检测文字候选框并由用户多选；Web 可由 Native Server 智能识别区域并继续手工拖动/缩放。
- **F4 OCR 识别** ✅：统一 `IOcrEngine` Port；Native Apple Vision 与 PaddleOCR/ORT Adapter
  均返回逐行 `OcrLine(text, confidence, box)`，保留 Native Mock 引擎用于测试。
- **F5 时间轴生成** ✅：双信号帧签名、变化点状态机与 SSIM patrol 生成 `start_ms` / `end_ms`。
- **F6 去重与合并** ✅：相邻同文去重、短空洞桥接与重叠处理；仍有少量 merged residual。
- **F7 字幕导出** ⚠️：SRT 格式化与单项下载已完成；ASS、VTT 只有接口占位，Web 工作区原子保存、冲突策略和真实批量导出由 12514 收口。
- **F8 CLI** ✅：Native `sublift extract <video> -o <output>`，支持 fps、置信度、OCR 引擎及
  文字系统参数。`--runtime` / `SUBLIFT_RUNTIME` 不是产品选项。Python console `sublift extract`
  已 fail-closed，指向 Native CLI。可选离线工具使用 `sublift-benchmark` /
  `python -m sublift_offline`，不占用产品 `sublift` 命令。
- **F9 中英文与混排** ⚠️：Vision 配置 zh-Hans + en-US，`auto/cjk/latin` 画像和多帧共识已实现；显式 CJK 边界清理对无空格混排仍有误删风险。
- **F10 引擎自动路由** ⚠️：GUI 可按候选文字推断文字系统，但尚未实现多 OCR 引擎自动选择。
- **F11 进度与取消** ⚠️：Native CLI 与 macOS GUI 已显示真实处理阶段和百分比并支持快速取消；Web 已有 SSE 与取消，但单视频百分比换算、重连 cursor 和刷新恢复尚由 12512 收口。
- **F12 配置文件** ❌：尚不支持 `sublift.toml` / `--config`。
- **F13 引擎对照模式** ❌：尚未提供产品化的多引擎并行对照。
- **F14 PaddleOCR 第二引擎** ✅：C++ ONNX Adapter 已实现完整 Det DB/unclip、
  Quad crop、Cls、Rec、metadata 字典与 CTC；stage、3 来源/614.272s 质量、性能、长流、
  cancel/restart 门均通过。`--engine paddle` 在 CLI/GUI 可选，模型齐备后离线推理；Native
  capability 缺失时 fail-closed，不进入 Python 或其他引擎。Phase 13 已完成模型目录、ORT
  来源和资源准备流程与 Python 生态的解耦。

### 3.2 Benchmark 与可观察性

- **F15 Benchmark 框架** ✅：隔离离线工具 `sublift-benchmark` 统一提供单组运行、
  通用参数矩阵、已有 SRT 评分和配置检查；默认提取后端为 Native CLI，冻结 Oracle
  仅在显式 `backend=oracle` 时加载。支持固定 GT、一对一对齐、timing/CER/usable/
  速度指标及 JSON/CSV/Markdown 诊断产物。评分层导入不加载 Pipeline/OCR/OpenCV/Pillow。
- **F16 时间轴诊断** ✅：`scripts/diagnostics/run_timeline.py` 与 benchmark failure clusters 可定位漏检、合并和误检。
- **F17 参数扫描** ✅：通用 `--set / --vary` 与 v2 config matrix 可组合 fps、engine、
  performance 等已注册参数，自动输出 matrix plan 和聚合 JSON/CSV/Markdown；历史算法
  专项脚本归入 `scripts/diagnostics/`。
- **F18 文字行诊断** ✅：Vision 保留逐行文字、置信度和位置，benchmark 可追踪行选择与共识结果。
- **F19 引擎列表命令** ❌：尚无 `sublift list-engines` 产品命令。

### 3.3 macOS GUI

- **F20 拖拽导入** ✅：支持 mp4 / mov / mkv。
- **F21 预览与实时结果** ✅：视频播放、时间定位、增量字幕和当前条目高亮。
- **F22 字幕编辑** ✅：可修改文本、合并/拆分条目；精细时间码编辑后置。
- **F23 字幕区域选择** ✅：Vision 候选框 + 用户多选；无选择时回退下部裁剪。
- **F24 批量处理队列** ✅（Phase 8 已实现）：独立 Task Center Window（⌘⇧T）支持多文件/文件夹发现、单并发可靠串行队列、安全 SRT 输出（skip/rename/replace）、本地 JSON 恢复与批量交互（筛选/多选/重排/显式改配置）。
- **F25 导出对话框** ✅：选择保存位置并导出 SRT。
- **F26 引擎管理 UI** ✅：vision / paddle / mock 选择通过 UserDefaults 持久化。
- **F27 独立 `.app` 分发** ❌：按 ADR-0009 跳过；当前通过 SwiftPM 构建运行。
- **F28 Native Workbench UI** ✅：Phase 10 已实施完成（2026-08-12）：单视频 Workspace、
  Context Inspector（Video/Region/Extraction/Subtitle）、只读 Live Transcript、可编辑 Review、
  轻量字幕时间线、分层 Settings 与快速提取设置栏；不含 F10 自动引擎或 F24 批量队列（non-goals）。

### 3.4 Web UI 与 Native Server

- **F29 Native Server / Web Workbench 基座** ✅：HTTP 206 视频流、代表帧、SSE、单视频
  工作台、ROI、增量字幕、Review 与 SRT 下载已实现；产品链不依赖 Python runtime。
- **F30 Web 智能字幕区域** ✅（12509）：Native Server 自动检测候选区域，Web 自动应用可信
  结果并允许手工覆盖；迟到检测不得覆盖用户已修改的选区。
- **F31 Web 批量任务中心与文件夹扫描** ⚠️（12510）：队列、筛选、Inspector、单项控制、
  浏览器文件夹导入和服务端路径扫描基座已存在；工作区内扫描、硬上限和完整 E2E 仍在收口。
- **F32 媒体沙箱与本地服务边界** ❌（12511）：WorkspaceManager 必须成为所有媒体路由
  的唯一授权根；默认 loopback，工作区外/符号链接逃逸和旧根路径必须 fail-closed。
- **F33 可恢复 Job/Queue 与可靠 SSE** ❌（12512）：队列、配置快照、事件 cursor 和终态结果
  需要版本化原子持久化；刷新/重启可恢复，活动任务转 interrupted，重连不重放字幕。
- **F34 单视频/批量配置与质量一致性** ❌（12513）：两个入口共享同一配置 DTO、默认值和
  `auto/fixed/default` ROI policy，实际生效配置可见且没有静默回退。
- **F35 真实输出计划与原子批量导出** ❌（12514）：服务端按受控路径和冲突策略原子写 SRT；
  浏览器下载与工作区保存分开，批量动作不能以重复下载伪装为已落盘。
- **F36 字幕审阅草稿与无损校对** ❌（12515）：文本/时间码编辑可恢复，具备保存状态、
  校验和撤销/重做；迟到事件不覆盖草稿，导出使用当前已确认版本。
- **F37 Web 可访问性与紧凑布局** ❌（12516）：核心路径具备键盘、焦点、dialog、状态播报、
  reduced motion、可选中文本和 960×600 / 200% zoom 验收合同。

## 4. 非功能需求

| 维度 | 要求 | 当前状态 |
|---|---|---|
| **性能** | 1080p、5fps 处理速度 ≥ 1× 实时 | ✅ Vision 固定 GT 实测 21.0×；Paddle 120s canonical C++ wall=Python×0.8956，且 720s 长流 15.522s wall |
| **离线 / 隐私** | 视频与文本不上传；模型齐备后不依赖网络 | ✅ Vision 与 Paddle 推理均在本机；Phase 13 已以固定 manifest/SHA 完成不依赖 Python 的 Native 模型与 ORT 准备闭环 |
| **资源占用** | 长视频处理保持流式，不随帧数线性增长 | ✅ 不累计完整视频帧；Paddle C++/Python canonical 进程树 RSS=0.9152x，720s Paddle C++ 长流通过；既有 4K/Vision 长流门也通过。 |
| **可分发** | 面向终端用户的独立安装包 | ❌ Phase 2 已明确跳过 `.app` 打包与公证 |
| **可扩展** | 能力模块依赖 Protocol，平台实现隔离 | ⚠️ 接口与分层完成，第三方插件发现机制未实现 |
| **当前兼容性** | 产品执行不要求 Python；macOS GUI 需 macOS 13+ 与 Swift 5.9+，Web/Server 使用受支持的 Native 平台 | ✅ Native CLI、macOS 与 Web 产品链均不依赖 Python；Linux 容器分发仍属于 12505 blocked 范围 |
| **运行时单一性** | C++ 是唯一产品运行时；缺失 capability 时 fail-closed，版本回滚不切 Python | ✅ ADR-0038 与 Phase 13 已完成并在 `main` 冻结 |
| **跨平台演进** | 核心算法与 OS / 厂商 API 解耦 | ⚠️ PaddleOCR 已提供通用 OCR 路径；Windows/Linux 产品交付与 GUI 尚未完成 |
| **本地服务信任边界** | Web Server 默认 loopback，当前工作区是所有媒体路径的唯一授权根 | ⚠️ Workspace UI 和 PathSandbox 已存在，但路由统一约束与工作区切换负例由 12510/12511 完成 |
| **任务可靠性** | Web job/queue 可恢复，SSE 可续传去重，进度单位一致 | ⚠️ 基础任务和 SSE 已实现；持久化、cursor、刷新/重启恢复和进度换算由 12512 完成 |
| **GUI 可用性 / 可访问性** | macOS 与 Web 主路径在紧凑布局、键盘、读屏和浅深色下可用 | ⚠️ macOS 代码与自动测试已实施，证据见 Phase 10；Web 的 focus/dialog/live-region/reduced-motion/960×600 合同由 12516 完成 |

## 5. 输入输出规范

### 输入

- 视频文件路径
- 可选配置：采样率、字幕区域、置信度、OCR 引擎、文字系统、合并与打轴参数

### 输出

- 当前产品输出 SRT；ASS / VTT 后置
- 处理日志与 benchmark 诊断产物（JSON / CSV / Markdown）

## 6. 不在当前范围内

- 软字幕轨提取、实时直播流、字幕翻译、云端 SaaS
- ASS / VTT 完整导出、配置文件（批量 GUI 队列已随 Phase 8 实现）
- 独立 `.app` 分发、公证，以及 Windows / Linux 产品交付
- 以 Python Pipeline、Python IPC 或进程内 runtime 切换作为产品回滚机制

## 7. 关键风险与权衡

| 风险 | 影响 | 缓解与状态 |
|---|---|---|
| GT 主要来自单一 Zootopia 片段 | 指标可能过拟合，不能代表泛化 | 后续扩充英文、中英混排、不同位置和不同片源 GT |
| 显式 CJK 边界清理误删无空格英文 | `NPD动物警局`、`苹果的iPhone` 等合法混排受损 | 默认使用 `auto`；风险记录于 HURDLES，待混排 GT 驱动机制修复 |
| 短字幕与 merged residual | 少量 timing FN 或合并错误 | 保留 failure cluster，后续按固定 GT/新增 GT 回归 |
| path mode 的抽帧与 OCR 串行 | OCR 时停止推进 ffmpeg；但 ROI 后并发重叠还会引入取消/重启竞态，且真实 Vision 未显示稳定 wall 收益 | Phase 4.1 已验证机制/质量/取消正确却未过 wall≤串行95% 门，故保持串行；Phase 4.2 已确认 Vision 请求执行主导，下一步先补多源 GT 后研究有效 OCR 调用。 |
| Apple Vision 在非 macOS/CI 不可用 | 集成覆盖受限 | Mock 闭环测试；平台 API 限定在适配层 |
| Native Paddle 资源来源或完整性漂移 | 模型/ORT 下载源、manifest 或 SHA 不一致会导致能力不可用或不可复现 | Phase 13 D03 已建立固定 manifest/SHA、临时下载与原子安装；校验失败 fail-closed，产品门持续验证资源身份 |
| Paddle GT 来源仍有限 | 现有 1 个真实 CJK + 2 个确定生成源不能代表所有片源 | 当前逐源 SHA exact 且全门通过；继续扩充真实 Latin/混排/不同字幕位置，不放宽冻结门 |
| ORT 二进制构建差异 | 相同版本/provider 仍可能有数值与性能差异 | Candidate 固定官方 ORT SHA；CMake 复制到 build `lib/` 并用相对 rpath；6.9+ 负责正式 App 签名/随包 |
| 退役 Python 后失去同版本 runtime fallback | Native capability 故障不能在当前版本内绕行 | fail-closed；回滚上一版已验收 Native artifact/tag；golden、GT 与 Native tests 承担回归真源 |
| 工作区配置未统一约束全部 HTTP 媒体路由 | 本地页面可读取或扫描工作区外文件，扩大隐私与误操作边界 | 12510/12511 统一 canonical root、symlink/旧根负例与 loopback 合同；完成前不宣称工作区是安全边界 |
| Web job/queue 与 SSE cursor 不可恢复 | 刷新或服务重启丢失队列，重连可能重复字幕或显示错误进度 | 12512 建立版本化有界持久化、interrupted 语义、Last-Event-ID/cursor 与 job id + seq 去重 |
| 单视频与批量入口默认配置漂移 | 同一视频因 ROI/质量档不同得到不可解释的字幕差异 | 12513 共享配置 DTO 与显式 ROI policy，并以 fixture exact + 真实 Vision 差异报告验收 |
| Web 输出路径与实际下载行为不一致 | UI 误报文件已保存，冲突提示与磁盘事实不符 | 12514 区分浏览器下载和工作区保存，以受控路径、冲突策略和原子写结果定义 completed |

## 8. 阶段验收状态

以下 Phase 1–13 条目记录各阶段当时的验收事实，不因 ADR-0038 追溯改写；其中关于 Python
Oracle/回滚的描述只表示历史状态，不再定义目标产品契约。

### Phase 1 — CLI MVP

- [x] CLI、ffmpeg 抽帧、Vision / PaddleOCR（可选）与 Mock、时间轴、去重和 SRT 导出闭环
- [x] Python lint、strict mypy、单元测试与 `./scripts/verify-standard.sh` 全绿（Harness L0 `./init.sh` 另行通过）
- [x] 真实视频端到端产出可加载 SRT

### Phase 2 — macOS GUI

- [x] 拖拽、预览、候选区域多选、字幕编辑与 SRT 导出
- [x] SwiftUI ↔ Worker UDS 闭环；默认 C++ Worker，显式 Python runtime 用于 Oracle / 回滚，mkv 预览走系统 ffmpeg
- [x] vision / paddle / mock 设置持久化
- [~] 独立 `.app` 分发与公证经用户决定跳过（ADR-0009）

### Phase 3 — 优化与基本可用

- [x] Benchmark 框架与统一 diagnostic 口径；feat-028 独立初始基线入库经用户决定跳过
- [x] 增量 `feed/ocr_segment/finalize`、`push_entry`、真实进度与快速取消
- [x] 固定 GT：timing recall 96.6%、precision 98.8%、F1 97.7%
- [x] 固定 GT：CER macro 3.2%、字符准确率 97.6%、usable 92.0%、noise/empty 0
- [x] Python、C++ 与 Swift 自动验证全绿
- [x] ≥10 分钟非 Zootopia GUI 手工体验验收已完成；多样化 GT 扩充留待后续

### Phase 4 — ROI 数据通路与真实长流验收（已完成）

- [x] 对有效固定字幕区域实现 ffmpeg crop-before-Python 的 ROI raw RGB 输出；不宣称 codec 级 ROI decode
- [x] 同提交 full / roi A/B：固定 GT detection hash 等价，raw bytes、图像构造成本、wall、RSS 与质量门均过验收
- [x] 用 ≥10 分钟非 Zootopia 硬字幕视频完成 path-mode（GUI 默认）首条、进度、取消、重启、导出和 RSS 真实体验验收
- [x] 性能计时归因：`pipeline_overhead` 已补齐；fake-clock、clean-commit Vision A/B、Python/Swift 验证均通过
- [ ] 英文/中英混排/不同字幕位置的 GT 扩充后置到下一质量泛化阶段

### Phase 4.1 — ROI 后可重叠流式吞吐（已归档，未采纳）

- [x] 实验实现的结果 hash、固定 GT、队列上限与取消/重启均通过
- [x] 两轮真实 Vision A/B 均未满足 end-to-end wall median ≤串行 95%（0.9559、1.0587），代码未合入 main
- [x] 保留串行 path mode；完整负向证据见 `docs/phases/phase41.json`

### Phase 4.2 — OCR 内部性能归因（已完成）

- [x] 记录 Vision 输入准备、request 设置、perform、observation 映射和 residual，且不与 `ocr` coverage leaf 双计
- [x] `trace` 记录有界的代表帧/调用/早停决策，不落盘文本、图像、box 或绝对路径
- [x] 自动对账硬门：summary/trace 内 `call_count == stages.ocr.count == throughput.ocr_calls`；parent 与外层 ocr wall 交叉校验；off 路径无分阶段计时
- [x] 归因证据已收口至 `docs/phases/phase42.json`：hash/质量/对账通过；`vision_perform≈99%`；下一方向为多源 GT 后的代表帧排序与有效调用实验
- [x] feat-043 已作为归因与优化分流任务收口；summary 扰动 1.319 未过，仅限制它不能作为产品速度基线。若未来需要此用途，另做交错 off/summary 配对复测

### Phase 5 — PaddleOCR 第二引擎（已完成）

- [x] Python rapidocr 接入、模型定位、质量与性能门完成
- [x] Paddle 作为 CLI/GUI 可选引擎，模型缓存后可离线运行

### Phase 6 — Native C++ Core（当前开发范围已完成）

- [x] vision / mock / paddle 默认 C++ Worker，协议与冻结 Oracle parity 通过
- [x] C++ capability 缺失时 fail-closed；显式 Python runtime 保留 Oracle / 开发回滚
- [x] Native target、Ports/Adapters、ResourceLocator 与开发期构建边界收口
- [ ] 独立 `.app`、依赖随包、签名、公证与最终发布 artifact Python-free 门后置到未来发布阶段

### Phase 8 — 批量任务中心与文件夹导入（已完成）

- [x] 08001：Task Center、文件/文件夹扫描、任务/队列状态、配置快照、输出冲突、持久化恢复与 B01–B10 验收合同已冻结
- [x] 08102–08207：领域模型、Scanner、Output Planner、串行 Scheduler、真实 Runner 与 JSON Repository
- [x] 08308–08309：独立 Task Center Window、批量交互、键盘与辅助功能
- [x] 08410：综合审核与验收（真实混合目录、真实串行 IPC 闭环、恢复/资源、B01–B10 证据矩阵、项目门与文档收口）

### Phase 10 — macOS Native Workbench UI（已完成）

- [x] 外部设计说明与 8 张参考图已整理为 `docs/design_ui/` 设计、状态、实现映射和资产基线
- [x] Task Center/批量队列、Automatic/Whisper、ASS/VTT、模型下载与分发已从本 Phase 排除
- [x] Workspace Session、Native Shell、Inspector、Region、Transcript、Processing/Review、Timeline、Settings 与快速提取设置栏已按 Feature 顺序实施
- [x] 完整 Swift 测试（201 XCTest + 140 Swift Testing）、项目标准门 10/10、V01–V09/960 紧凑/Light-Dark 截图及 A01/A02 代码与自动测试证据通过
- [ ] V10 Increase Contrast/Reduce Transparency 实际切换、完整 VoiceOver 朗读会话和部分真实点击路径仍受系统权限限制；替代覆盖与边界记录于 10412–10415 evidence

### Phase 12 — Cross-Platform Web UI & Native Server（in-progress）

- [x] 12001–12509：Native Server、HTTP/SSE、单视频 Workbench、ROI/Review、批量基座、媒体工作区、智能区域检测与本地验证基线
- [>] 12510：Web 批量任务中心交互增强与受控文件夹扫描（当前唯一进行中 Feature）
- [ ] 12511–12516：统一信任边界、可恢复任务、配置/质量一致性、真实导出、审阅草稿和 Web 可访问性
- [~] 12505：Linux 容器与 Paddle 完整分发暂缓，保持 blocked；不阻塞本地 Web/Native Server 收口

### Phase 13 — Python Runtime 退役与 Native-only 收口（已完成）

- [x] ADR-0038 已确认唯一 Native 产品运行时、fail-closed 与版本回滚边界
- [x] Python 产品 CLI、IPC、runtime 路由及宿主接线已移除；算法副本冻结为隔离 Oracle
- [x] 产品安装、构建、启动、提取、导出与 Native 日常验证在无 Python / `.venv` 环境通过
- [x] 模型与 ORT 资源准备脱离 Python 生态，并以固定 manifest/SHA 验收
- [x] 最后 Python 产品 revision 以 Git tag `python-product-last` 保存；工作树不建立源码 archive
- [x] 可选 benchmark/评分/Oracle 使用独立命名空间、依赖组与 `./scripts/verify-offline.sh`
