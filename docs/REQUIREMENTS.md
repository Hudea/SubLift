# SubLift 需求规格

## 1. 项目目标

为桌面用户提供一个**本地、离线、免费**的硬字幕提取工具，把烧录在视频画面中的字幕还原为可编辑字幕。当前已交付 Python CLI 与 macOS SwiftUI 开发者版本；PaddleOCR 已作为可选 CLI/GUI 引擎接入，但 Windows/Linux 产品交付与 GUI 仍是后续范围。

**Phase 6.0–6.9 开发范围已完成：** 产品执行路径已迁移到 C++ Core，并与冻结 Python
Oracle 对齐；vision/mock/Paddle 均已完成 C++ cutover。Paddle C++ 不可用时
fail-closed；仅显式 `SUBLIFT_RUNTIME=python` 进入 Oracle/开发回滚。`.app` 分发、签名、
公证按 ADR-0030 后置。迁移契约见 [`docs/cpp/`](cpp/README.md)。

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
- **F2 帧采样** ✅：按可配置采样率抽帧，默认 5fps；GUI path mode 由实际选中的
  C++ / Python Worker 使用各自 `FfmpegExtractor`，两端遵循冻结的抽帧 parity 契约。
- **F3 字幕区域** ✅：CLI 默认裁剪画面下部 30%，支持固定区域；GUI 用 Vision 检测文字候选框并由用户多选字幕框。
- **F4 OCR 识别** ✅：统一 `OcrEngine` 接口；Apple Vision 与 PaddleOCR（rapidocr PP-OCRv6）均返回逐行 `OcrLine(text, confidence, box)`，保留 Mock 引擎用于测试。
- **F5 时间轴生成** ✅：双信号帧签名、变化点状态机与 SSIM patrol 生成 `start_ms` / `end_ms`。
- **F6 去重与合并** ✅：相邻同文去重、短空洞桥接与重叠处理；仍有少量 merged residual。
- **F7 字幕导出** ⚠️：SRT 已完成；ASS、VTT 只有接口占位。
- **F8 CLI** ✅：`sublift extract <video> -o <output>`，支持 fps、置信度、OCR 引擎及文字系统参数。
- **F9 中英文与混排** ⚠️：Vision 配置 zh-Hans + en-US，`auto/cjk/latin` 画像和多帧共识已实现；显式 CJK 边界清理对无空格混排仍有误删风险。
- **F10 引擎自动路由** ⚠️：GUI 可按候选文字推断文字系统，但尚未实现多 OCR 引擎自动选择。
- **F11 进度与取消** ✅：CLI/GUI 显示真实处理阶段和百分比；GUI 可快速终止 ffmpeg 与后台任务并重新开始。
- **F12 配置文件** ❌：尚不支持 `sublift.toml` / `--config`。
- **F13 引擎对照模式** ❌：尚未提供产品化的多引擎并行对照。
- **F14 PaddleOCR 第二引擎** ✅：C++ ONNX adapter 已实现完整 Det DB/unclip、
  Quad crop、Cls、Rec、metadata 字典与 CTC；stage、3 来源/614.272s 质量、性能、长流、
  cancel/restart 与回滚门均通过。Python rapidocr 保留为 Oracle/fallback。
  `--engine paddle` CLI/GUI 可选，首次模型下载后离线推理，模型缓存位于
  `~/.cache/sublift/rapidocr-models`。

### 3.2 Benchmark 与可观察性

- **F15 Benchmark 框架** ✅：`sublift-benchmark` 统一提供单组运行、通用参数矩阵、
  已有 SRT 评分和配置检查；支持固定 GT、一对一对齐、timing/CER/usable/速度指标及
  JSON/CSV/Markdown 诊断产物。
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
- **F24 批量处理队列** ❌：当前一次处理一个视频。
- **F25 导出对话框** ✅：选择保存位置并导出 SRT。
- **F26 引擎管理 UI** ✅：vision / paddle / mock 选择通过 UserDefaults 持久化。
- **F27 独立 `.app` 分发** ❌：按 ADR-0009 跳过；当前通过 SwiftPM 构建运行。

## 4. 非功能需求

| 维度 | 要求 | 当前状态 |
|---|---|---|
| **性能** | 1080p、5fps 处理速度 ≥ 1× 实时 | ✅ Vision 固定 GT 实测 21.0×；Paddle 120s canonical C++ wall=Python×0.8956，且 720s 长流 15.522s wall |
| **离线 / 隐私** | 视频与文本不上传；已缓存模型时不依赖网络 | ✅ Vision 全程本机处理；PaddleOCR 仅首次下载模型需要联网，之后本机推理 |
| **资源占用** | 长视频处理保持流式，不随帧数线性增长 | ✅ 不累计完整视频帧；Paddle C++/Python canonical 进程树 RSS=0.9152x，720s Paddle C++ 长流通过；既有 4K/Vision 长流门也通过。 |
| **可分发** | 面向终端用户的独立安装包 | ❌ Phase 2 已明确跳过 `.app` 打包与公证 |
| **可扩展** | 能力模块依赖 Protocol，平台实现隔离 | ⚠️ 接口与分层完成，第三方插件发现机制未实现 |
| **当前兼容性** | macOS 13+，Python 3.12+；GUI 需 SwiftPM/Xcode | ✅ macOS 开发者环境可构建运行；PaddleOCR 是不依赖 Vision 的可选 OCR 引擎 |
| **跨平台演进** | 核心算法与 OS / 厂商 API 解耦 | ⚠️ PaddleOCR 已提供通用 OCR 路径；Windows/Linux 产品交付与 GUI 尚未完成 |

## 5. 输入输出规范

### 输入

- 视频文件路径
- 可选配置：采样率、字幕区域、置信度、OCR 引擎、文字系统、合并与打轴参数

### 输出

- 当前产品输出 SRT；ASS / VTT 后置
- 处理日志与 benchmark 诊断产物（JSON / CSV / Markdown）

## 6. 不在当前范围内

- 软字幕轨提取、实时直播流、字幕翻译、云端 SaaS
- ASS / VTT 完整导出、配置文件、批量 GUI 队列
- 独立 `.app` 分发、公证，以及 Windows / Linux 产品交付

## 7. 关键风险与权衡

| 风险 | 影响 | 缓解与状态 |
|---|---|---|
| GT 主要来自单一 Zootopia 片段 | 指标可能过拟合，不能代表泛化 | 后续扩充英文、中英混排、不同位置和不同片源 GT |
| 显式 CJK 边界清理误删无空格英文 | `NPD动物警局`、`苹果的iPhone` 等合法混排受损 | 默认使用 `auto`；风险记录于 HURDLES，待混排 GT 驱动机制修复 |
| 短字幕与 merged residual | 少量 timing FN 或合并错误 | 保留 failure cluster，后续按固定 GT/新增 GT 回归 |
| path mode 的抽帧与 OCR 串行 | OCR 时停止推进 ffmpeg；但 ROI 后并发重叠还会引入取消/重启竞态，且真实 Vision 未显示稳定 wall 收益 | Phase 4.1 已验证机制/质量/取消正确却未过 wall≤串行95% 门，故保持串行；Phase 4.2 已确认 Vision 请求执行主导，下一步先补多源 GT 后研究有效 OCR 调用。 |
| Apple Vision 在非 macOS/CI 不可用 | 集成覆盖受限 | Mock 闭环测试；平台 API 限定在适配层 |
| PaddleOCR 首次模型下载失败 | 首次 `--engine paddle` 无法启动 | CLI 给出不含 traceback 的失败原因与预下载命令；模型缓存后离线复用 |
| Paddle GT 来源仍有限 | 现有 1 个真实 CJK + 2 个确定生成源不能代表所有片源 | 当前逐源 SHA exact 且全门通过；继续扩充真实 Latin/混排/不同字幕位置，不放宽冻结门 |
| ORT 二进制构建差异 | 相同版本/provider 仍可能有数值与性能差异 | Candidate 固定官方 ORT SHA；CMake 复制到 build `lib/` 并用相对 rpath；6.9+ 负责正式 App 签名/随包 |

## 8. 阶段验收状态

### Phase 1 — CLI MVP

- [x] CLI、ffmpeg 抽帧、Vision / PaddleOCR（可选）与 Mock、时间轴、去重和 SRT 导出闭环
- [x] Python lint、strict mypy、单元测试与 `./scripts/verify-standard.sh` 全绿（Harness L0 `./init.sh` 另行通过）
- [x] 真实视频端到端产出可加载 SRT

### Phase 2 — macOS GUI

- [x] 拖拽、预览、候选区域多选、字幕编辑与 SRT 导出
- [x] SwiftUI ↔ Python UDS 闭环，mkv 走系统 ffmpeg
- [x] vision / paddle / mock 设置持久化
- [~] 独立 `.app` 分发与公证经用户决定跳过（ADR-0009）

### Phase 3 — 优化与基本可用

- [x] Benchmark 框架与统一 diagnostic 口径；feat-028 独立初始基线入库经用户决定跳过
- [x] 增量 `feed/ocr_segment/finalize`、`push_entry`、真实进度与快速取消
- [x] 固定 GT：timing recall 96.6%、precision 98.8%、F1 97.7%
- [x] 固定 GT：CER macro 3.2%、字符准确率 97.6%、usable 92.0%、noise/empty 0
- [x] Python 与 Swift 自动验证全绿
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
- [x] 正式报告 `docs/reports/phase4.2-ocr-attribution-baseline.md`：hash/质量/对账通过；`vision_perform≈99%`；下一方向为多源 GT 后的代表帧排序与有效调用实验
- [x] feat-043 已作为归因与优化分流任务收口；summary 扰动 1.319 未过，仅限制它不能作为产品速度基线。若未来需要此用途，另做交错 off/summary 配对复测
