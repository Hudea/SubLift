# SubLift

硬字幕（烧录字幕）提取工具——从视频画面中自动识别字幕，生成可编辑的 SRT 文件。

本地运行、隐私优先，视频与识别文本不离开本机。Phase 13 的目标产品运行时统一为
Native C++；Vision 与准备好模型的 PaddleOCR 均在本机离线推理。

## 特性

- **Apple Vision OCR**：C++/ObjC++ 产品实现；默认中英双语识别（zh-Hans + en-US）
- **PaddleOCR 跨平台引擎**：PP-OCRv6 + Native ONNX Runtime，缺少能力或模型时 fail-closed
- **像素差异打轴**：双信号帧签名（前景占比 + dHash）+ 三态状态机，时间轴稳定
- **OCR 后置与段内共识**：每段最多识别 4 个代表帧，按字幕画像选行并用跨帧共识抑制背景文字
- **模块化可插拔**：extractor / detector / ocr / export 均为 Protocol，可替换实现
- **真实进度与快速取消**：CLI/GUI 展示处理阶段和百分比，GUI 可中途取消并重新开始
- **macOS GUI**：SwiftUI 界面，拖拽导入、视频预览、增量字幕、字幕编辑、SRT 导出
- **批量任务中心** ✅：独立 Task Center（⌘⇧T）、多文件/文件夹统一导入、单并发串行队列、安全 SRT 输出与本地 JSON 恢复（Phase 8 已完成）

## 环境要求

- macOS 13+（当前 GUI 与 Vision 引擎；Apple Silicon 推荐）
- ffmpeg（含 ffprobe）
- CMake 3.20+ 与 C++20 工具链（推荐 Ninja）
- Xcode 15+ 或 SwiftPM（仅 GUI 构建需要）
- Node.js 20+（仅 Web UI 构建需要）

Python 3.12+ 与 `uv` 当前只服务仍在迁移的 benchmark、诊断、历史 Oracle 和兼容代码；
Phase 13 完成后，它们不是产品安装条件，也不得成为 Native-only 产品运行或最终门禁的依赖。

## 文档

- [架构](docs/ARCHITECTURE.md)
- [需求规格](docs/REQUIREMENTS.md)
- [Native C++ 架构与契约](docs/cpp/README.md)
- [当前进度](progress.md)与 [Phase 索引](phases.json)
- [CHANGELOG 6.6](CHANGELOG.md) — 历史 cutover 与质量/性能记录
- [项目辅助架构](docs/phases/phase7.json) — Phase 7 统一保存 Harness 迁移及原 Phase 9 仓库治理记录；Phase 9 已释放

## 安装

```bash
git clone <repo>
cd SubLift
./init.sh                  # 开工基线：连续性入口与 Phase detail JSON 链
cmake -S cpp -B build/cpp -G Ninja -DCMAKE_BUILD_TYPE=Debug -DSUBLIFT_REQUIRE_OPENCV=ON -DSUBLIFT_ENABLE_VISION=ON
cmake --build build/cpp
ctest --test-dir build/cpp --output-on-failure
```

`./scripts/verify-standard.sh` 仍是当前可用的全仓验证入口，但处于 Phase 13 迁移态：
它还会同步 Python 依赖并运行历史 Oracle/parity 测试。不得据此把 Python 解释为产品依赖；
在 Native 门完全收口前，应如实保留这项过渡限制。

### 可选的过渡工具

当前本地产物清理器仍由 Python 实现。它不是产品命令；默认只预览可重建产物及预计释放
空间，不写盘：

```bash
uv run python scripts/cleanup_local_artifacts.py
```

Swift build、C++ build 与 Python 工具环境必须分别显式选择；`.venv` 不在推荐默认范围：

```bash
uv run python scripts/cleanup_local_artifacts.py --swift-build
uv run python scripts/cleanup_local_artifacts.py --cpp-build
uv run python scripts/cleanup_local_artifacts.py --python-env
```

工具只接受当前 Git checkout 内的固定路径，拒绝仓库根、Git 元数据、路径逃逸、符号链接、
视频、SRT/GT 与未分类生成文件。`--apply` 会执行实际删除；在真实工作区使用前必须先取得
用户的再次明确授权。它从不清理分支、worktree、模型、外部资源或固定本地媒体。

### 原生 C++ 构建（vision/mock 产品路径）

```bash
cmake -S cpp -B build/cpp -G Ninja -DCMAKE_BUILD_TYPE=Release -DSUBLIFT_ENABLE_VISION=ON
cmake --build build/cpp
# 产物：build/cpp/bin/sublift（与 sublift_cli）+ sublift_worker
```

> Vision 未安装时，OCR 集成测试自动跳过，pipeline 可用 MockOcrEngine 跑闭环测试。
> Native Paddle 当前要求模型文件已存在于 `SUBLIFT_PADDLE_MODEL_DIR` 或现有默认目录；
> 缺少模型时明确失败，不会下载模型或切换到 Python。

### Paddle Native Release 构建

下面是当前历史验收环境仍可复现的命令。它从 Python wheel 取得 ORT dylib，属于
Phase 13 必须清除的过渡债务，不是 Native-only 最终构建合同：

```bash
ORT_CAPI="$PWD/.venv/lib/python3.12/site-packages/onnxruntime/capi"
cmake -S cpp -B build/cpp-rel -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DSUBLIFT_ENABLE_OPENCV=ON -DSUBLIFT_REQUIRE_OPENCV=ON \
  -DSUBLIFT_ENABLE_PADDLE=ON -DSUBLIFT_REQUIRE_PADDLE=ON \
  -DONNXRuntime_INCLUDE_DIR="$(brew --prefix onnxruntime)/include/onnxruntime" \
  -DONNXRuntime_LIBRARY="$ORT_CAPI/libonnxruntime.1.28.0.dylib"
cmake --build build/cpp-rel
```

CMake 会把所选 ORT 复制到 `build/cpp-rel/lib/`，并给 Worker 写入相对
`@loader_path/../lib`。Phase 13 需要进一步改为独立、固定 SHA 的 Native ORT 与模型资产，
并移除 `.venv`、RapidOCR 缓存布局及 Python 版本对 Native 构建的影响。完整 `.app` 内置
模型、签名、公证仍按 ADR-0030 后置。

## 使用

### 原生 C++ CLI（无需 uv）

```bash
./build/cpp/bin/sublift extract <video> -o output.srt
./build/cpp/bin/sublift extract clip.mkv --fps 5 --script cjk -o out.srt
./build/cpp/bin/sublift extract clip.mkv --engine mock -o out.srt
./build/cpp-rel/bin/sublift extract clip.mkv --engine paddle -o out.srt  # 使用上面的 Paddle Release 构建
```

Phase 6.8 的 Python/C++ 对照结果属于迁移历史，不再定义产品运行时。Phase 13 起产品只
接受 C++ Worker：能力不可用时 fail-closed；需要回滚时回滚到上一已验收版本，而不是在
同一版本内切换 Python 实现。仓库中尚存的 Python CLI、IPC 路由和环境变量是待移除债务，
不构成受支持用法。

### Runtime 矩阵

| engine | 产品 runtime | 说明 |
|---|---|---|
| **vision** | **C++** (`sublift_worker`) | macOS 主路径 |
| **mock** | **C++** | 流程验证 |
| **paddle** | **C++** | 完整 DB/Quad/Cls/Rec Native；能力或资产缺失即报错 |

### CLI 参数

| 参数 | 默认值 | 说明 |
|---|---|---|
| `<video>` | 必填 | 输入视频路径 |
| `-o, --output` | output.srt | 输出字幕文件路径 |
| `--fps` | 5.0 | 帧采样率（推荐 5.0） |
| `--confidence` | 0.5 | OCR 高置信门；低置信文本仅在多帧共识等条件满足时放行 |
| `--engine` | vision | OCR 引擎（vision / paddle / mock）；Paddle 需要已准备好的 Native 模型资产 |
| `--script` | auto | 字幕文字系统（auto / cjk / latin）；已知字幕语言时可显式指定 |

## macOS GUI（开发者构建）

> Phase 2 GUI 当前通过 SwiftPM 构建运行，**不做独立 `.app` 分发包**（见 ADR-0009）。
> Phase 13 的 GUI 产品合同只启动 C++ `sublift_worker`。现存 Python runtime 分支属于
> 迁移债务，不是支持的 GUI 模式。

```bash
cd apps/macos
swift build
swift run SubLiftMac
```

### GUI 功能

- **拖拽导入**：把 mp4 / mov / mkv 视频拖入窗口
- **视频预览**：AVPlayer 播放，支持播放/暂停/拖动进度条
- **字幕区域选择**：Vision 自动检测文字候选框，多选字幕框后提取（无选择时回退下部裁剪）
- **实时反馈**：段闭合后增量显示字幕，处理阶段、百分比和相对实时处理倍速均来自真实帧进度
- **快速取消**：提取中可终止 ffmpeg 与后台任务，取消后可重新开始
- **原生工作台**：Video / Transcript Split、可选 Context Inspector、系统 Toolbar 与菜单命令
- **字幕审阅**：处理期只读 Live Transcript；完成后搜索、选择、修改、合并/拆分并通过时间线定位
- **提取设置**：视频区快速设置栏与分层 Settings 共用 vision / paddle / mock（开发者模式）和采样质量偏好
- **SRT 导出**：点击「导出 SRT」选择保存路径

> 处理 mkv 需要系统已安装 ffmpeg，否则 UI 会提示 `brew install ffmpeg`。
>
> Phase 10 Native Workbench 已实施并收口；当前能力、视觉证据与已知交互限制见
> [UI 设计入口](docs/design_ui/README.md) 和 [Phase 10 跟踪](docs/phases/phase10.json)。
>
> Phase 8 Task Center 已完成（08001 设计冻结 + 08102–08410 实现与综合验收）；实施与证据见
> [批量任务中心设计合同](docs/design_ui/batch-task-center.md) 与 [Phase 8 跟踪](docs/phases/phase8.json)。

## 开发

```bash
./init.sh                     # 开工基线，会话开始/结束时运行
cmake --build build/cpp
ctest --test-dir build/cpp --output-on-failure
(cd apps/macos && swift test)
npm --prefix apps/web test
./scripts/verify-standard.sh  # 当前过渡态全仓验证；仍包含 Python Oracle 检查
```

仅在维护隔离的 Python benchmark、诊断或历史 Oracle 时才运行：

```bash
uv run pytest -m "not integration" --no-cov
uv run ruff check .
uv run mypy src tests
```

### Harness 与进度

`phases.json → docs/phases/phase*.json` 是当前开发 Phase 和 Deliverable 的操作真源；
`.agent/` 当前只保留轻量规则、会话入口和提交辅助，复杂能力编排已从活跃 Harness 移出，
后续按真实需要增量引入。根 `feature-list.json` 仍保留给历史文档和旧工具兼容，不能用于
选择新工作。执行 Task 只存在于当前会话计划，不写入长期 Phase 文件。详见
[AGENTS.md](AGENTS.md) 与 [ADR-0033](docs/DECISIONS.md)。

### 架构与设计

- [架构设计](docs/ARCHITECTURE.md) — 模块布局、数据流、分层原则
- [需求规格](docs/REQUIREMENTS.md) — 功能需求、非功能需求、验收标准
- 设计文档：[UI vNext](docs/design_ui/README.md) · [pipeline](docs/design/pipeline.md) · [ocr](docs/design/ocr.md) · [extractor](docs/design/extractor.md) · [benchmark](docs/design/benchmark.md) · [macos-gui 当前实现](docs/design/macos-gui.md) · [OCR 内部性能归因（Phase 4.2 计划）](docs/design/ocr-performance-attribution.md)
- [Benchmark 用法](benchmark/README.md) — 统一 `run/matrix/score` 入口、产物与回归锚点
- [已知障碍](docs/HURDLES.md) — 开发中遇到的技术问题与解决方案

### Phase 3 固定 GT 水位

Zootopia 固定片段（1080p、5fps、统一 diagnostic 口径）的最终结果：timing recall 96.6%、precision 98.8%、F1 97.7%，CER macro 3.2%（字符准确率 97.6%），usable subtitle recall 92.0%，空文本与噪声均为 0。该结果用于回归锚点，不代表对其他片源的泛化保证；非 Zootopia 长视频 GUI 手工体验验收已在 Phase 4 完成，但英文/中英混排/不同字幕位置的量化 GT 扩充仍是后续工作。

隔离的 Python Benchmark 当前仍使用 `uv run sublift-benchmark`；入口与指标说明见
[benchmark/README.md](benchmark/README.md)（设计见
[docs/design/benchmark.md](docs/design/benchmark.md)）。已验收的
[质量与性能归因基线](benchmark/baselines/README.md)随仓库版本化。版本化配置引用的固定
本地媒体保留在 `debug/` 根目录且不入库；GUI/C++ 导入、运行、性能报告与历史归档统一写入
`debug/benchmark/`。该工具可以辅助分析 Native 输出，但不得成为产品运行或最终 Native
门禁依赖。

### 技术栈

C++20 / ObjC++ / CMake / Ninja / Swift 5.9+ / SwiftPM / Vue 3 / TypeScript / OpenCV /
ONNX Runtime / ffmpeg；Python 3.12+ 与 uv 仅用于隔离的过渡工具。
