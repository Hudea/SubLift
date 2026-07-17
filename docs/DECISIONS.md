# 架构决策记录

记录项目中已确认的技术决策，按时间倒序排列。  
**`progress.md` 只保留最近 3 条决策，旧条目归档至此。**

---

## ADR-0015 性能 coverage 以排他 Pipeline 编排阶段补齐（2026-07-17）

- **状态**：已确认并已实现（feat-041）。
- **背景**：ROI A/B 的某次实测中，`frame_materialize` 已显著下降，但
  `stage_coverage_pct` 有一轮为 98.996%。约 121ms 的 core wall 未归入现有 leaf stages，
  使总耗时差异无法可靠解释；这段时间来自 `run_frames` 的帧迭代、状态机/Timeline 分派、
  容器阶段自身与 recorder 固定开销，而非 ROI 像素处理或 OCR 结果变化。
- **决策**：
  1. 新增 `pipeline_overhead` 为 coverage leaf stage；它测量 `Pipeline.run_frames()`
     的外层 wall，并扣除其中互不重叠的既有 leaf stages。
  2. `finalize` 继续作为容器 stage 排除在 coverage 外；其未嵌套编排成本自然归入
     `pipeline_overhead`，而内部 `ocr` / `dedupe` 仍仅计一次。
  3. 新增 `PerformanceRecorder.exclusive_span()` 作为通用排他计时 API；使用方必须只传入
     彼此不重叠的子阶段。
- **理由**：以“外层 wall − 内层 leaf”记录编排时间，既能使 coverage 可审计，又不改变
  ROI、打轴或 OCR 的实际执行路径；把未归因时间静默忽略或降低门槛都会削弱性能结论。
- **结果**：fake-clock 证明无双计；clean commit 1b4612b 的 canonical Vision A/B 中，ROI
  三次 coverage 为 99.999778% / 99.999784% / 99.999730%，全部硬门与软目标通过。

---

## ADR-0014 固定字幕区域自动在 ffmpeg 输出前裁剪，Pipeline 只消费 frame-local 坐标（2026-07-17）

- **状态**：已确认并已实现（feat-038/039/040）；ROI filter 为 `fps,format=rgb24,crop:exact=1`。
- **背景**：feat-037 canonical baseline 显示全帧 raw RGB 输出约 7.91 GB；其中
  extract_wait 约 24.1%、frame_materialize 约 14.8%。GUI 默认 path mode 已只发送
  视频路径和固定 region，Python 端却仍让 ffmpeg 输出完整 1920×1080 RGB，随后 Pipeline
  再裁 [0,848,1920,87] 字幕带。
- **决策**：
  1. 有效固定 region 的 GUI path mode 与 benchmark ROI 组，使用 ffmpeg
     fps,crop(...:exact=1)，只把 ROI raw RGB 经 stdout 传入 Python。
  2. source-frame region 只用于 ffmpeg 与诊断；ROI Frame 进入 Pipeline 后一律改用
     frame-local 全幅 Region [0,0,w,h]，由 RoiPassthroughDetector 明确表达，禁止把
     source box 二次用于图像 crop。
  3. GUI 不增加 ROI 开关；显式 ROI 非法时任务报错，region 为空、legacy frame mode、
     BottomCrop 与未验证旋转映射维持现有全帧路径。
  4. benchmark 保留仅供内部使用的 full / roi A/B 输出模式；ROI 结论必须与同次
     detection_hash、固定 GT 质量门和环境元数据一起报告。
  5. 本决策不宣称 codec 级 ROI decode；H.264 / HEVC 等通常仍需完整重建编码帧。
- **理由**：ROI 输出能以确定比例消除无效 RGB 搬运，却不改打轴/OCR 算法；局部坐标
  detector 使 source / ROI 坐标不会静默混用，便于测试与回退。
- **影响**：涉及 extractor、detector、pipeline、bridge 与 benchmark；CLI BottomCrop、
  JPEG/缩放、并发、旋转映射和跨片源质量泛化均不纳入本轮。完整契约见
  docs/design/roi-data-path.md。

---

## ADR-0013 SSIM patrol 为内部默认机制，不暴露给 GUI 用户（2026-07-13）

- **背景**：feat-031 A/B 验证时 GUI 接入了「SSIM 巡逻」开关。产品稳定后该开关仍留在主界面与设置页；且 Swift 关闭时发送 `nil` 而非 `false`，Python 继续用默认 `True`，开关形同虚设。
- **决策**：
  1. 删除主界面与设置页的 SSIM 巡逻开关；GUI 不再传 `enable_ssim_patrol`。
  2. 产品路径统一使用后端 `ChangePointConfig.enable_ssim_patrol=True`。
  3. IPC 可选字段保留，供 benchmark、回归测试与内部诊断显式 `true`/`false`。
  4. 若将来需要 GUI 调试入口，仅放在 DEBUG 开发者设置，且关闭时必须发送明确的 `false`。
- **理由**：SSIM patrol 是打轴质量的内部补强，不是用户可选偏好；暴露半失效开关只会制造假控制与支持成本。
- **结果**：GUI 提取始终走默认开启 patrol；诊断路径仍可显式关闭。

---

## ADR-0012 增量 pipeline、真实进度与取消共享任务生命周期（2026-07-12）

- **背景**：Phase 2 批量模式会先缓存全部帧再统一处理，用户长时间看不到字幕；取消只改变 bridge 状态，无法解除 worker 在 ffmpeg 读取上的阻塞。CLI 与 GUI 也缺少一致、真实的阶段进度。
- **决策**：
  1. `Pipeline` 以 `feed(frame)`、`ocr_segment(segment)`、`finalize()` 支持逐帧推进，段闭合后立即通过 IPC `push_entry` 推送。
  2. GUI path mode、CLI 与 benchmark 共享 Python `FfmpegExtractor`；以视频时长和采样率估算总帧，报告 `processing/finalizing` 与实际帧计数。
  3. 单次任务共享持久化 cancellation event；取消同时终止 ffmpeg 子进程，worker 在 `finally` 中释放 extractor 并回收线程，使下一任务可重新启动。
  4. Vision OCR 调用使用 `autorelease_pool`，图像桥接数据使用 CFData 生命周期，避免长流临时对象累积。
- **理由**：首条反馈、进度真实性、快速取消和内存稳定性本质上属于同一处理生命周期，必须由同一状态与资源所有权约束，不能靠 UI 假进度或仅设置布尔标记补偿。
- **结果**：自动审计首条反馈 0.68s、取消响应 0.108s，ffmpeg 进程退出且第二任务可正常启动；4K 流式内存审计通过。≥10 分钟非 Zootopia GUI 手工体验验收由用户决定暂缓。

---

## ADR-0011 OCR 文字系统默认 auto，CJK 横幅按边界清理（2026-07-10）

- **背景**：feat-034 初版为清理 `PHISON/SON` 使用无条件拉丁尾缀正则，误删纯英文和合法中英混排；其相似文本聚类又未把簇票数传给接受策略，低置信中文字幕仍被清空。
- **决策**：
  1. 无显式 `SubtitleProfile` 时文字系统默认 `auto`；GUI 按选中候选文本推断 `cjk/latin/auto`，CLI/benchmark 可显式固定。
  2. 共识结果必须携带真实 `support_votes`，低置信接受策略直接消费簇票数，不再下游按全文精确相等重算。
  3. 仅在显式 CJK 画像下清理与 CJK 边界直接粘连的拉丁横幅；纯英文、空格分隔英文及中文内部缩写均保留。
- **理由**：让语言假设来自用户选区或显式配置，以多帧证据处理 OCR 变体，并把水印清理限制在可解释的结构边界内。
- **结果**：固定 GT usable 92.0%、CER macro 3.2%、noise/empty 0、timing F1 97.7%、precision 98.8%；合法英文与 `ZPD` 有回归测试。

---

## ADR-0010 打轴抽帧统一到 Python FfmpegExtractor（2026-07-09）

- **背景**：GUI 用 AVF+JPEG 推帧时，同 region/Config 下 timing F1 ~84–86%，而 CLI/benchmark live（`FfmpegExtractor`）达 95.2%（5fps）/ 96.4%（8fps）。日志证明 region 与 pipeline 默认参数一致，差在选帧相位、时间戳网格与 JPEG 有损。
- **决策**：
  1. **打轴采样唯一实现** = Python `FfmpegExtractor`（与 CLI / `run_benchmark` 同源）。
  2. GUI 默认 **path mode**：`start_job.video_path` 传本地路径，后端自抽帧；Swift **不再**为打轴推 JPEG frame 流。
  3. 保留 **frame mode**（无 `video_path`）作兼容/调试。
  4. AVF 仅用于 **预览与选区代表帧**，与打轴解耦。
- **理由**：验收与产品必须同一像素/时间戳序列；双端各抽帧必然漂移。
- **影响**：`bridge.py` path mode；`SubtitleExtractor` 默认 path；`docs/design/macos-gui.md` 抽帧章节；后续默认 fps 仍建议 5（速度），8 作高精度档。

---

## ADR-0009 移除 Phase 2 .app 打包与 notarization 流程（2026-07-06）

- **背景**：feat-025 原计划把 SwiftUI 工程打包为可分发 `.app`，并完成 Developer ID 签名 + `notarytool` 公证，使 Gatekeeper 放行。该流程需要 Apple Developer Program 会员、Developer ID 证书、embedded Python 运行时以及 hardened runtime 适配。
- **决策**：**跳过 feat-025**，不做 `.app` 打包、embedded Python、签名与公证。Phase 2 当前范围止于「可在开发者环境通过 SwiftPM 构建并运行」的 macOS GUI，不产出面向终端用户的独立 `.app` 分发包。
- **理由**：用户明确决定移除该流程；当前阶段优先完成 GUI 功能与文档收尾，避免引入证书、公证、embedded Python 等分发侧阻塞。
- **影响**：
  - `docs/phases/phase2.json` 中 feat-025 状态改为 `blocked`，并注明跳过原因；feat-026 依赖从 feat-025 改为 feat-024。
  - `feature-list.json` 中 `phase2.distribution` 状态改为 `blocked`。
  - `README.md` 与相关文档不再包含 `.app` 安装 / 分发段，仅描述 SwiftPM 构建运行方式。
  - 未来如需分发，可重新开启 feat-025 或作为独立 release 工程处理。

---

## ADR-0008 feat-022 字幕区域：Vision 候选框 + 用户多选（2026-07-05）

- **背景**：feat-022 原计划为用户在预览上手动画矩形并重新跑流水线。用户反馈手动画框负担高、画错易导致识别失败；HURDLES 已记录 `bottom_ratio=0.3` 裁太宽问题，方案 3 为 Vision 自适应区域检测。
- **决策**：
  1. **取消手动画框**；改为代表帧上 **Vision 自动检测全部文字候选框**，预览以**彩色编号框**叠加展示。
  2. 用户**多选**哪些候选框属于字幕（可排除新闻标题等误检）；支持多行字幕对应多框。
  3. **区域推算**：**Y** 取选中框 min~max（加 padding）；**X MVP 固定全宽**（`x=0, width=video_width`）。
  4. 检测与预览叠加在 **Swift 端**用 `VNRecognizeTextRequest` 实现（feat-022a）；合并后的 `region_box` 接入 `start_job` + `bridge` `FixedRegionDetector`（feat-022b，已完成）。
- **理由**：兼顾自动化与用户可控；比纯启发式合并更抗误检；比手动画框更低门槛。X 全宽避免裁掉长字幕行。
- **影响**：`RegionPicker`(手画) 改为 `RegionOverlay`+`RegionCandidateList`；`docs/phases/phase2.json` feat-022 重写；`docs/plans/phase2.md` 区域相关段落已同步（§6.2/§6.3/§7/§8.3/§9）。

---

## ADR-0007 Phase 2 GUI 三项设计决策（2026-07-04）

Phase 2 启动前对三项影响 feat-015/016/024 的设计点拍板：

### ADR-0007a Pipeline 帧流接入：新增 `run_frames()`（feat-016）

- **背景**：Phase 1 `Pipeline.run(video_path: Path)` 内部调 `self._extractor.extract(video_path)`，假设帧来自视频文件。Phase 2 GUI 模式下帧由 Swift 端 AVFoundation/ffmpeg 抽取后经 IPC 以 JPEG 字节流送入 Python，无 `video_path`。plan §5.3 原写「小幅改造由 feat-016 评估」，未定方案。
- **决策**：在 `Pipeline` 上新增 `run_frames(frames: Iterator[Frame]) -> list[SubtitleEntry]` 方法，与 `run(video_path)` 并列。`run(video_path)` 内部重构为先用 extractor 生成 frames 迭代器再调 `run_frames`，避免逻辑重复。`bridge.py` 接收 IPC JPEG 帧后重建 `PIL.Image` 组装 `Frame`，调 `run_frames`。
- **理由**：这是真实的接口需求（GUI 帧源不是文件），「Python 核心零修改」是 Phase 2 起初的理想化约束，最小侵入的扩展方法优于 bridge 重写编排逻辑（C 方案）或假 path 包装（B 方案）。`run_frames` 与 `run` 共享内部步骤，不破坏 Phase 1 CLI 行为。
- **影响**：`src/sublift/pipeline/core.py` 新增 `run_frames` 并重构 `run`；feat-016 bridge 直接调 `run_frames`；Phase 1 测试需保持通过（`run` 行为不变）。更新 `docs/plans/phase2.md` §5.3 与 `docs/phases/phase2.json` feat-016 描述。

### ADR-0007b SRT 导出：Swift 端直接实现（feat-024）

- **背景**：feat-024 原写「经 IPC 调 Python SrtExporter 或 Swift 端直接调 sublift.export 模块，双方案内选一」。编辑后的字幕条目已存在于 Swift 内存，走 IPC 往返 Python 无收益。
- **决策**：SRT 格式化在 Swift 端实现。Swift 维护 `entries: [SubtitleEntry]` 模型，导出时本地格式化为 SRT 文本写文件。
- **理由**：SRT 格式极简（序号 + `HH:MM:SS,mmm --> HH:MM:SS,mmm` + 文本 + 空行），无理由走 IPC 往返；且避免引入「为导出再发 IPC 请求」的状态机分支。
- **影响**：`apps/macos/Sources/SubLiftMac/Core/SrtFormatter.swift` 新增；不调 Python 导出。更新 `docs/phases/phase2.json` feat-024 描述。

### ADR-0007c/d MsgPack 评估与撤销（独立决策，2026-07-05）

- **背景**：feat-015 原计划在 IPC 层引入 MsgPack 替换 JSON（plan §4.2）。评估两个 Swift MsgPack 库后，发现都有嵌套解码 bug（详见 HURDLES）。
- **决策**：**不引入 MsgPack，继续用 JSON**（ADR-0007c 撤销引入，ADR-0007d 确认 JSON 为最终方案）。
- **影响**：此决策**独立于 feat-015 任务范围**。feat-015 仍需定义 7 类消息的 JSON schema + 双向单测，只是序列化层用 JSON 而非 MsgPack。feat-015 因此从「MsgPack 序列化」重定义为「IPC 协议 7 类消息 schema（JSON 序列化）」。

---

## ADR-0006 Phase 2 抽帧双方案：AVFoundation 主路径 + 系统 ffmpeg 兜底（2026-07-03）

- **背景**：Phase 2 GUI 需要从视频抽帧后通过 IPC 送 Python Pipeline。AVFoundation 是 macOS 原生方案，可利用 VideoToolbox 硬解且无需额外依赖；但 feat-012 spike 证明 AVFoundation 无法打开 mkv 容器（报错 `-11828` / `-12847`），而 SubLift 需要支持 mkv 输入。
- **决策**：
  1. **主路径**：mp4 / mov / H.264 / HEVC 走 `AVAssetReader` + `AVAssetReaderTrackOutput`，`CVPixelBuffer` → `CGImage` → JPEG q=0.85。
  2. **兜底路径**：`.mkv` 及 AVFoundation 拒绝的容器走系统 `ffmpeg`，命令 `ffmpeg -i input -vf fps=5 -f image2pipe -vcodec mjpeg -`，stdout 为 MJPEG 流，按 SOI/EOI marker 切帧。
  3. **运行时检测**：启动时 `which ffmpeg` 检测，缺失弹窗引导 `brew install ffmpeg`，UI 禁用 mkv 拖入。
- **理由**：AVFoundation 覆盖 macOS 最常见格式且零额外依赖；ffmpeg 是 mkv 的通用解。按扩展名路由简单可靠，避免 AVFoundation 失败后再回退的 ~1s 延迟。
- **影响**：
  - `apps/macos/Sources/SubLiftMac/Core/FrameSampler.swift` 负责 AVFoundation 路径。
  - `apps/macos/Sources/SubLiftMac/Core/FfmpegFallback.swift` 负责 ffmpeg 检测、MJPEG 切帧、mkv 抽帧与元数据探测。
  - `apps/macos/Sources/SubLiftMac/Core/VideoMetadata.swift` 对 mp4/mov 用 AVURLAsset，对 mkv 用 ffprobe。
  - `docs/plans/phase2.md` §5 更新为双方案描述。

---

## ADR-0005 Phase 2 GUI 架构：SwiftUI 壳 + Python UDS 子进程（2026-07-03）

- **背景**：Phase 1 已交付可运行的 CLI Pipeline（Python）。Phase 2 需要 macOS GUI，目标是复用 Phase 1 算法核心，避免重写。
- **决策**：
  1. **双工程布局**：`apps/macos/` 新建 SwiftUI 工程，`src/sublift/` Python 工程保持不变。
  2. **进程边界**：SwiftUI 主进程作为 UI 壳，通过 **Unix Domain Socket (UDS)** 与本机启动的 Python 子进程通信。
  3. **Phase 1 核心零修改**：`pipeline/` / `extractor/` / `detector/` / `ocr/` / `export/` / `models.py` / `config.py` 不改动；新增 `src/sublift/ipc/` 模块把 Pipeline 包装为 IPC handler。
  4. **序列化**：JSON（4 字节大端长度前缀分帧），见 ADR-0007c/d。
- **理由**：
  - 复用已验证的 Python 算法核心，降低 GUI 阶段风险。
  - UDS 是本地进程间通信的轻量方案，不依赖网络，Swift 与 Python 都原生支持。
  - 明确的分层边界为 Phase 3 跨平台留路：核心算法与 UI 壳解耦，未来可用 Tauri/Electron 替换 SwiftUI 而不动 Python 核心。
- **影响**：
  - `apps/macos/Sources/SubLiftMac/Core/PipelineClient.swift` 负责启动 Python 子进程与 UDS 通信。
  - `src/sublift/ipc/server.py` / `protocol.py` / `bridge.py` 负责 Python 端 IPC 服务。
  - `Pipeline.run_frames()` 新增（ADR-0007a），让 bridge 可以注入 Swift 送来的 JPEG 帧流。
  - `docs/ARCHITECTURE.md` 增加 Phase 2 章节描述双工程 + IPC。

---

## ADR-0004 任务粒度调整：22 细任务合并为 10 粗任务（2026-07-03）

- **背景**：初始 Phase 1 规划拆出 22 个细粒度任务（feat-001~022），实践中发现粒度过细，导航与跟踪成本高。
- **决策**：合并为 10 个粗任务（feat-001~010），每个粗任务 = 一个可独立验收的功能块；任务内部细节通过 phaseN.schema.json 新增的 `subtasks` 字段承载（name + description），不拆成独立任务。CLI extract 实现随 feat-009/010 接入；端到端真实视频验收作为 Phase 1 级验收门，不单列任务。feat-004（文档）移到末尾依赖 feat-010，确保实现稳定后再写文档。
- **理由**：粒度以「能否独立验收」为准；粗任务承载主验收，subtasks 承载实现细节，兼顾可跟踪性与导航效率。
- **影响**：`docs/phases/phaseN.schema.json` 新增 `subtasks` 字段；`docs/phases/phase1.json` 重写为 10 任务 + 17 subtasks；`feature-list.json` covers 对齐；`docs/plans/phase1.md` 任务表与执行顺序图重写。ADR-0001/0003 中对旧 task ID 的引用以本决策为准。

## ADR-0001 Phase 1 模块布局与分层（2026-07-03）

- **背景**：Phase 1 需建立项目底座，定义字幕提取的三能力模块与串联层。
- **决策**：采用「三能力模块 + 串联层 + 入口层」分层。
  - 能力模块：`detector`（字幕区域检测）、`extractor`（帧采样）、`ocr`（OCR，可插拔多引擎）。每个模块定义 Protocol + 默认实现，可热插拔。
  - 串联层：`pipeline`（端到端编排，含 timeline 变化点状态机与 dedupe 去重合并）、`export`（字幕导出，Phase 1 实现 SRT，ASS/VTT 留接口占位）。
  - 入口层：`cli`（argparse，零依赖起步，`sublift extract` 子命令）。
- **理由**：用户明确三模块划分（detector/extractor/ocr）；extractor 定位为「帧采样器」而非端到端编排器；串联逻辑由 pipeline 承担。模块低耦合高内聚，平台特定 API 只能出现在 `ocr/vision.py`，核心层只依赖 Protocol，为 Phase 3 跨平台留路。
- **影响**：`docs/plans/phase1.md` §2 模块布局、§5 抽象接口；`feature-list.json` 7 大功能块；`docs/phases/phase1.json` 任务跟踪（任务粒度后经 ADR-0004 调整）。

## ADR-0002 Apple Vision 经 PyObjC 桥接接入（2026-07-03）

- **背景**：Phase 1 需选定 OCR 默认引擎的接入路径。
- **决策**：Python 通过 `pyobjc-framework-Vision` 直接调用 `VNRecognizeTextRequest`，不引入 Swift helper 子进程。
- **理由**：Phase 1 是 CLI 核心定位，纯 Python 路径最快；PyObjC 桥接避免额外的构建/分发复杂度；Swift 留给 Phase 2 GUI。
- **影响**：`pyobjc-framework-Vision` 与 `pyobjc-framework-Quartz` 作为 macOS 可选依赖；`ocr/vision.py` 是平台特定 API 唯一容身处；导入失败需优雅降级提示。

## ADR-0003 Phase 1 范围确定为可运行 MVP（2026-07-03）

- **背景**：Phase 1「底座」的深度需明确，决定验收标准与工作量。
- **决策**：Phase 1 交付可运行 MVP——真实 1080p 视频经 `uv run sublift extract <video> -o out.srt` 产出可加载 SRT；而非仅抽象接口。
- **理由**：用户选择「可运行 MVP」选项。端到端可运行才能验证架构有效性，避免抽象底座脱离实际。
- **影响**：`docs/phases/phase1.json` 验收标准含真实视频产出 SRT 与三工具全绿；端到端验收作为 Phase 1 级验收门（任务粒度见 ADR-0004）。ASS/VTT、PaddleOCR 第二引擎、配置文件、进度展示等显式排除。
