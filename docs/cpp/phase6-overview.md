# Phase 6 — Native C++ Core Migration（总览）

> 设计源头（本文件）+ 契约（`docs/cpp/*.md`）+ 子阶段文档  
> 任务跟踪：`docs/phases/phase6.json`  
> 编号规范：`docs/cpp/NAMING.md`  
> 项目级功能块：`feature-list.json` → `phases[phase6]`

## 1. 主题与动机

**主题：** 在**不推翻**现有模块边界的前提下，将 Python Runtime 逐步替换为 **C++ Core**；  
SwiftUI 与 UDS/JSON IPC **暂时保留**，先把 Python Worker 换成 C++ Worker（按引擎矩阵）。

**动机（是 / 不是）：**

| 是 | 不是 |
|---|---|
| 原生 `.app` 不再依赖嵌入 Python | 指望 Vision OCR 从 ~50ms 变 ~10ms |
| CLI / GUI / 未来跨平台共享同一 native core | 边迁移边改打轴/OCR 算法 |
| 数据结构、线程与内存所有权更清晰 | 第一阶段上 libav / Swift↔C++ 直连 |
| 后续接 ONNX/Paddle 原生 runtime 更自然 | 大爆炸式「把每个 .py 译成 .cpp」 |

Phase 4.2 已表明：canonical ROI 下 Vision `performRequests` 占 OCR parent ~99%。  
C++ 化是 **产品与架构收口**，不是 OCR 热路径微优化。

## 2. 目标架构与 Target 图

高层数据流：

```text
SwiftUI ──UDS/JSON──► sublift-worker (C++ 或按引擎选 Python)
                         │
CLI ─────────────────────►│  lib targets（见 architecture.md）
                         ▼
                    SubtitleEntry
Python ── 仅冻结 Oracle / benchmark / Paddle fallback（见引擎矩阵）
```

**可执行依赖图与工具链锁定**见 [architecture.md](architecture.md)（`sublift_core` / `sublift_ffmpeg` / `sublift_vision_macos` / `sublift_worker` / `sublift_cli` / `sublift_test_support`）。

硬约束：

1. `sublift_core` **不得**依赖 Swift、Objective-C、Apple Vision。
2. Vision 仅存在于 `sublift_vision_macos`（`.mm`）。
3. 第一阶段 FFmpeg **保持 subprocess**，不上 libav。
4. 行为以 **冻结 Oracle** 为准（`oracle_commit` + 环境 + golden），**不是**「未钉扎的当前 main 尖端」。见 [parity-contract.md](parity-contract.md)。

## 3. 子阶段路线图

| 子阶段 | 前缀 | 目标 | 产品路径 |
|---|---|---|---|
| **6.0 Bootstrap** | `feat-060xx` | 文档、CMake targets、models/完整 Config、parity 冻结、**IPC/引擎/cutover 契约** | **零切换** |
| **6.1 Pure pipeline** | `feat-061xx` | signature → … → line_select golden parity | 仍 Python |
| **6.2 Pipeline** | `feat-062xx` | C++ `feed` / `ocr_segment` / `finalize` / `cancel` | 仍 Python |
| **6.3 Extractor** | `feat-063xx` | ffmpeg subprocess + ROI + 索引时间戳 | 仍 Python |
| **6.4 Vision** | `feat-064xx` | ObjC++ Vision adapter | 仍 Python |
| **6.5 Worker** | `feat-065xx` | 按 [worker-ipc-contract.md](worker-ipc-contract.md) 实现 | 可双轨 |
| **6.6 Cutover** | `feat-066xx` | 按 [engine-matrix-and-cutover.md](engine-matrix-and-cutover.md) 默认化 | vision/mock→C++；paddle→Python |
| **6.7 Paddle Native MVP** | `feat-067xx` | [phase6.7-paddle.md](phase6.7-paddle.md)：ONNX PP-OCRv6 `IOcrEngine` 可运行、接线与基础 golden | 当前实现可用时自动 C++；真实质量/性能未过产品门 |
| **6.8 Paddle hardening** | `feat-068xx` | [phase6.8-paddle-hardening.md](phase6.8-paddle-hardening.md)：完整 Det/Cls/Rec parity、多源质量、性能、长流与回滚 | **done：available→C++ stable** |
| **6.9+** | `feat-069xx`… | 去 Python 产品依赖 / 打包分发等 | 后置 |

**进入 6.1 的硬门槛：** `feat-06001`–`feat-06005` 全部 `done`。

## 4. 全局验收原则

```text
冻结 Oracle (commit + env + golden_schema)
        │
        ├─ Python dump  ─→ golden / result_A
        └─ C++ Candidate ─→ result_B
                │
                └─ L0–L2 exact/epsilon → L3 GT 水位 → 运行时门 → cutover
```

- 中间量（signature、events、段边界、代表帧、OCR 决策）优先于「只比 SRT」。
- 固定 GT 参考水位见 benchmark 文档（F1/CER/usable 等）。
- Cutover **另含** cancel/restart、wall/RSS、ASan、回滚开关——见引擎/cutover 契约。

## 5. 技术栈

见 [architecture.md §2.1](architecture.md)（C++20、CMake、Catch2、nlohmann/json、OpenCV 可选、ffmpeg 可执行文件）。

**明确不引入（6.x 默认）：** Boost、Qt、libav*、重型 DI/async 框架。

## 6. 与 Phase 5 / Paddle 的关系

- Phase 5.0（PaddleOCR via rapidocr）**已完成**（Python Oracle）。
- C++ **6.0–6.6 不实现**原生 Paddle；cutover 后 **paddle 显式走 Python worker**，vision/mock 走 C++。
- **6.7** 已落地 C++ adapter（ONNX Runtime + PP-OCRv6），证明 Native MVP 可运行；其
  synthetic golden 与 L4 文本检查不等于 RapidOCR 真实推理质量 parity。
- **6.8** 已完成完整 Det/Cls/Rec parity、多源 Paddle GT、性能门与产品重新 cutover。
  设计：[phase6.8-paddle-hardening.md](phase6.8-paddle-hardening.md)。完整矩阵：见
  [engine-matrix-and-cutover.md](engine-matrix-and-cutover.md)。

## 7. 非目标（全 Phase 6 默认）

- 不把 `.py` 逐文件机械翻译当成功标准。
- 不在迁移中「顺手」修 signature 色域、时间戳语义、行选阈值。
- 不上 libav、不做 Swift C++ interop 单进程合并。
- 不删除仓库内 Python（oracle / benchmark / Paddle fallback）。
- 不把完整 Phase 4.2 归因树作为 6.0–6.5 必达项。

## 8. 契约索引（P1）

| 主题 | 文档 |
|---|---|
| Target / 图像 / Config | [architecture.md](architecture.md) |
| Oracle / golden / rounding | [parity-contract.md](parity-contract.md) |
| Worker 时序与所有权 | [worker-ipc-contract.md](worker-ipc-contract.md) |
| 引擎矩阵与 cutover/回滚 | [engine-matrix-and-cutover.md](engine-matrix-and-cutover.md) |

## 9. 当前状态

- **6.0 Bootstrap：** **done** — [phase6.0-bootstrap.md](phase6.0-bootstrap.md)
- **6.1 Pure pipeline：** **done** — [phase6.1-pure-pipeline.md](phase6.1-pure-pipeline.md)
- **6.2 Pipeline 流式编排：** **done** — [phase6.2-pipeline.md](phase6.2-pipeline.md)（feat-06201–06205）
- **6.3 FFmpeg Extractor：** **done** — [phase6.3-extractor.md](phase6.3-extractor.md)（feat-06301–06305）
- **6.4 Vision OCR：** **done** — [phase6.4-vision.md](phase6.4-vision.md)（feat-06401–06405）
- **6.5 C++ Worker：** **done** — [phase6.5-worker.md](phase6.5-worker.md)（feat-06501–06505；opt-in 双轨）
- **6.6 Cutover：** **done** — [phase6.6-cutover.md](phase6.6-cutover.md)（feat-06601–06605）
- **6.7 Paddle C++ Native MVP：** **done** — [phase6.7-paddle.md](phase6.7-paddle.md)（feat-06701–06706）
- **6.8 Paddle Native hardening：** **done** — [phase6.8-paddle-hardening.md](phase6.8-paddle-hardening.md)（feat-06801–06807）
- **当前实现：** vision/mock → C++；Paddle available → C++ stable，unavailable → Python Paddle fallback；`SUBLIFT_RUNTIME=python` 可回滚
- **原生 CLI：** `build/cpp/bin/sublift`（或 `sublift_cli`）`extract`；`uv run sublift` 保留为 oracle / 回滚 / paddle 无 native 时
- **残差风险：** Paddle Q2 为 1 个真实 CJK + 2 个确定生成源，仍需扩充真实 Latin/混排；
  正式 `.app` 内置 ORT/模型、签名公证属于 6.9+；Release Vision synthetic 用例的既有
  环境失败不属于 Paddle cutover。
