# C++ Core 架构（Phase 6 冻结草案）

> 状态：6.0 设计冻结（随 `feat-06002`/`06003` 实现校对，接口破坏须回写本文）。  
> 总览：`phase6-overview.md` · 启动：`phase6.0-bootstrap.md` · 编号：`NAMING.md`

## 1. 当前产品架构 vs 目标架构

| | 当前（Phase 5 末，产品默认） | 目标（Phase 6.6 cutover 后） |
|---|---|---|
| GUI | SwiftUI → UDS → **Python** worker | SwiftUI → UDS → **C++** worker |
| CLI | `uv run sublift` → Python Pipeline | **`sublift` 原生可执行**（同 core）；Python CLI 可作 oracle |
| OCR | Vision/Paddle/Mock（Python） | Vision/Mock（C++，6.6）；Paddle C++ adapter（6.7 ONNX）见 [engine-matrix-and-cutover.md](engine-matrix-and-cutover.md) / [phase6.7-paddle.md](phase6.7-paddle.md) |
| 抽帧 | Python FfmpegExtractor subprocess | C++ `sublift_ffmpeg` subprocess（同语义） |
| 行为 oracle | — | **冻结 commit + 环境 + golden**，非「漂移中的 main」 |

## 2. CMake target 依赖图（`feat-06002` 必须落地）

```text
                    ┌─────────────────────┐
                    │  sublift_test_support │  golden IO / compare
                    └──────────▲──────────┘
                               │
┌──────────────┐    ┌──────────┴──────────┐    ┌────────────────────┐
│ sublift_cli  │───►│    sublift_core     │◄───│ sublift_ffmpeg     │
└──────────────┘    │ models/config       │    │ subprocess extract │
                    │ interfaces          │    └────────────────────┘
┌──────────────┐    │ pipeline/export     │
│sublift_worker│───►│ (no ObjC/Vision)    │◄─── IOcrEngine
└──────┬───────┘    └─────────────────────┘         ▲
       │                      ▲                     │
       │                      │            ┌────────┴───────────┐
       │                      │            │ sublift_vision_macos│
       │                      │            │ .mm  only           │
       │                      │            └────────┬───────────┘
       │                      │                     │
       │                      │            ┌────────┴───────────┐
       │                      │            │ sublift_paddle     │
       │                      │            │ ONNX Runtime       │
       │                      │            │ (6.7, option OFF)  │
       └──────────────────────┼────────────┴────────────────────┘
```

| Target | 允许依赖 | 禁止 |
|---|---|---|
| **`sublift_core`** | C++20 STL、（可选）OpenCV **仅作图像缓冲实现细节若在 core 暴露则通过 `ImageBuffer` 抽象** | ObjC、Swift、Vision、ORT、UDS、系统 ffmpeg 进程封装可放 core 接口但实现宜在 `sublift_ffmpeg` |
| **`sublift_ffmpeg`** | `sublift_core`、POSIX spawn/pipe | Vision、ORT、IPC |
| **`sublift_vision_macos`** | `sublift_core`、Apple Vision/Quartz | 被 Linux 构建默认链接；ORT |
| **`sublift_paddle`** | `sublift_core`、**ONNX Runtime**（系统包） | ObjC、Vision、UDS；**不得**反向依赖 worker |
| **`sublift_worker`** | core + ffmpeg +（macOS）vision +（可选）paddle；JSON；UDS | Swift |
| **`sublift_cli`** | core + ffmpeg + ocr adapters | GUI |
| **`sublift_test_support`** | core；读写 golden | 产品路径 |

`feat-06002` 至少创建 **可链接的 target 壳**（空实现可），并在 `cpp/README.md` 画出上表。未实现的 target 用 `SUBLIFT_ENABLE_VISION=OFF` 等选项关掉。

### 2.1 工具链锁定（06002 验收项）

| 项 | 6.0 冻结选择 | 备注 |
|---|---|---|
| 语言 | C++20 | `-std=c++20` |
| 构建 | CMake ≥ 3.20 + CTest | out-of-source `build/cpp` |
| 测试框架 | **Catch2 v3**（FetchContent 或系统包） | 与 GTest 二选一后**禁止并行**；选定 Catch2 |
| JSON | **nlohmann/json**（FetchContent） | Worker 与 golden 共用 |
| OpenCV | **软可选** `find_package(OpenCV QUIET)`；`SUBLIFT_ENABLE_OPENCV` 默认 ON，**找不到则自动关闭** signature（models/image 仍可编）；硬失败用 `SUBLIFT_REQUIRE_OPENCV=ON`。core 图像类型见 §3，不在 API 表面强制 `cv::Mat` | 6.1+ 本机开发推荐 `brew install opencv@4`；CI/无 OpenCV 环境不硬挂 configure |
| 链接 | 默认 **静态** `sublift_*` 进 worker/cli；系统 OpenCV 可 dynamic | 避免产品静默混用两套 OpenCV ABI |
| Symbol visibility | 默认 hidden；仅 C API（若有）显式 export | 为日后 `.app` 做准备 |
| 依赖获取 | FetchContent 仅用于 Catch2 + nlohmann；OpenCV/ffmpeg/ORT **系统安装** | 不 vendor 整棵 OpenCV/ORT |
| Paddle OCR（6.7） | `SUBLIFT_ENABLE_PADDLE` 默认 **OFF**；ON 时 `find` ONNX Runtime + `sublift_paddle` | 见 [phase6.7-paddle.md](phase6.7-paddle.md) |
| Sanitizer | Debug 可选 `SUBLIFT_SANITIZE=ON` → ASan+UBSan | cutover 门使用 |

## 3. 图像与几何契约（`feat-06003` 前冻结，禁止占位后换类型）

### 3.1 `ImageBuffer` / `ImageView`

```text
ImageBuffer  — 拥有像素内存（unique ownership）
ImageView    — 非拥有；指向 Buffer 或外部只读区；不得长于 Buffer 生命周期
```

| 规则 | 约定 |
|---|---|
| 像素格式 | 显式枚举：`RGB24` / `BGR24` / `Gray8`（可扩展） |
| 布局 | row-major；`stride_bytes >= width * bpp`；可非连续 |
| 只读 | `ImageView` 默认只读像素；写入仅通过 `ImageBuffer` 或明确 `ImageViewMut` |
| ROI | **共享 view**（指针+偏移+宽高+stride），不默认拷贝；需稳定快照时显式 `clone()` |
| 与 OpenCV | 允许 **实现内部** 从 `ImageView` 构造 `cv::Mat` header（不拥有）；**禁止** 在公共 API 以 `cv::Mat` 作为模块边界类型 |
| 线程 | 同一 `ImageBuffer` 不得并行写；跨线程传递 view 须保证 buffer 存活（job 线程模型见 worker 契约） |
| ffmpeg 输出 | path mode ROI raw = **RGB24**（与现 Python pipe 一致）；signature 路径的「BGR 再当 RGB gray」属 **算法层怪癖**，不在此改语义 |

### 3.2 `Frame`

```text
struct Frame {
  int64_t timestamp_ms;   // 与 Python int 对齐；索引时间戳语义
  ImageBuffer image;      // 或 ImageView + 明确 owner 在 Pipeline 会话内
};
```

- **整数宽度**：时间戳与几何字段统一 **`int32_t` 像素几何**（x/y/w/h，与 Python `int` 像素一致且非负约束）、**`int64_t timestamp_ms`**（避免长片溢出歧义；序列化/JSON 仍用 number）。
- 不在 6.0 用 `std::vector<uint8_t>` 无格式元数据的「裸占位」作为跨模块类型。

### 3.3 强类型坐标盒（推荐 06003 落地）

避免仅靠注释区分坐标空间：

| 类型 | 坐标空间 |
|---|---|
| `SourceBox` | source-frame（全帧 / GUI region / ffmpeg crop 参数） |
| `FrameLocalBox` | 当前 extractor 输出图（ROI 后即 ROI 图） |
| `OcrCropBox` | 传入 `recognize` 的图（与 `OcrLine.box`） |

`Region` 携带 `FrameLocalBox`；IPC/manifest 入参为 `SourceBox`，在 extractor/detector 边界转换。

### 3.4 完整 `Config`（不可只迁 Signature/ChangePoint）

C++ 必须覆盖 Python `Config` 全字段（默认值与 `src/sublift/config.py` 一致），包括但不限于：

| 字段 | 默认（Python） |
|---|---|
| `sample_fps` | 5.0 |
| `region_bottom_ratio` | 0.3 |
| `confidence_threshold` | 0.5 |
| `merge_gap_ms` | 1000 |
| `min_duration_ms` | 500 |
| `ocr_anchor_delay_frames` | 2 |
| `drop_empty_text` | false |
| `subtitle_profile` | nullopt |
| `subtitle_script` | `"auto"` |
| `enable_line_select` | true |
| `low_conf_threshold` | 0.28 |
| `ocr_consensus_frames` | 4 |
| `line_select_min_score` | 0.28 |
| `line_select_min_script` | 0.12 |
| `signature` | `SignatureConfig` 默认 |
| `change_point` | `ChangePointConfig` 默认 |

嵌套 `SignatureConfig` / `ChangePointConfig` 全字段同样锁定。单测对 **完整默认 Config 快照** 做字段级比对（可 golden JSON）。

## 4. 能力接口（方向；详细签名随 6.1–6.3 实现微调但不得破坏依赖方向）

```text
IExtractor::extract(path, opts) -> stream of Frame
IDetector::detect(Frame) -> Region
IOcrEngine::recognize(ImageView) -> OcrResult   // 非 cv::Mat 公共参数
IExporter::format/export(entries)
Pipeline::feed / ocr_segment / finalize / cancel
```

- OCR **串行**、同一 Pipeline instance 不并行 recognize（与现架构一致）。
- 取消：协作式 flag + 终止 ffmpeg 子进程（worker 层）。

## 5. 错误模型（概要）

| 层 | 策略 |
|---|---|
| core | 可恢复业务用 `Result`/`expected` 或错误码；不抛过边界除非文档约定 |
| worker | 映射为协议 `done(ok=false)` / `error` + 可读 message；**不**静默换引擎 |
| CLI | 非 0 退出 + stderr 可操作文案 |

## 6. 与 Python 设计文档的关系

- `docs/design/pipeline.md` 等在 cutover 前仍是 **行为语义 oracle 叙述**。
- 本文 + `parity-contract.md` 是 **C++ 结构与验收 oracle**。
- 冲突时：以 **冻结的 golden + oracle_commit 行为** 为准，再回头改文档。

## 7. 部署约束（P2 提前登记，影响 CMake 布局）

详见 cutover 文档；摘要：

- Worker 二进制进 `.app` 的 `Contents/MacOS/` 或 `Helpers/`
- rpath / 签名 / 公证 / 最低 macOS / universal2 在 6.7 实现，但 **visibility 与静态链接默认** 在 6.0 按上表设
- ffmpeg：6.x 默认 **系统 PATH**；随包分发另 feat
