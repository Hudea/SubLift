# SubLift

硬字幕（烧录字幕）提取工具——从视频画面中自动识别字幕，生成可编辑的 SRT 文件。

本地运行、隐私优先，视频与识别文本不离开本机。Vision 可直接离线使用；PaddleOCR 首次下载模型后也在本机离线推理。

## 特性

- **Apple Vision OCR**：C++/ObjC++ 为产品路径，PyObjC 保留 Oracle；默认中英双语识别（zh-Hans + en-US）
- **PaddleOCR 跨平台引擎**：PP-OCRv6 + ONNX Runtime；C++ Native 为产品默认，
  Python rapidocr 保留为 Oracle 与一键回滚
- **像素差异打轴**：双信号帧签名（前景占比 + dHash）+ 三态状态机，时间轴稳定
- **OCR 后置与段内共识**：每段最多识别 4 个代表帧，按字幕画像选行并用跨帧共识抑制背景文字
- **模块化可插拔**：extractor / detector / ocr / export 均为 Protocol，可替换实现
- **真实进度与快速取消**：CLI/GUI 展示处理阶段和百分比，GUI 可中途取消并重新开始
- **macOS GUI**：SwiftUI 界面，拖拽导入、视频预览、增量字幕、字幕编辑、SRT 导出

## 环境要求

- macOS 13+（当前 GUI 与 Vision 引擎；Apple Silicon 推荐）
- Python 3.12+
- ffmpeg（含 ffprobe）
- uv（包管理）
- Xcode 15+ 或 SwiftPM（仅 GUI 构建需要）

## 文档

- [架构](docs/ARCHITECTURE.md)
- [需求规格](docs/REQUIREMENTS.md)
- **[Phase 6 C++ 迁移与 cutover](docs/cpp/README.md)**（6.0–6.8 已完成；Paddle Native 已正式 cutover）
- **[Phase 6.8 C++ Paddle 回顾索引](docs/cpp/phase6.8-review-index.md)**（问题审计、设计、逐项修改、ADR、验收与复跑入口）
- [CHANGELOG 6.6](CHANGELOG.md) — 默认切换、回滚、质量/性能摘要
- [Harness 迁移架构（历史）](docs/plans/architecture/harness-migration.md) — Phase 7 初次迁移记录；当前精简边界见 AGENTS 与 ADR-0033

## 安装

```bash
git clone <repo>
cd SubLift
./init.sh                  # 开工基线：连续性入口与 Phase detail JSON 链
./scripts/verify-standard.sh  # 完整日常验证：lint/测试/C++/parity
uv sync --extra vision     # 仅需单独补装 Vision 依赖时使用
uv sync --extra paddle     # 仅需单独补装 PaddleOCR 依赖时使用
```

### 本地产物卫生

默认只预览可重建的 L1 生成产物及预计释放空间，不写盘：

```bash
uv run python scripts/cleanup_local_artifacts.py
```

Swift build、C++ build 与 Python 环境必须分别显式选择；`.venv` 不在推荐默认范围：

```bash
uv run python scripts/cleanup_local_artifacts.py --swift-build
uv run python scripts/cleanup_local_artifacts.py --cpp-build
uv run python scripts/cleanup_local_artifacts.py --python-env
```

工具只接受当前 Git checkout 内的固定路径，拒绝仓库根、Git 元数据、路径逃逸、符号链接、
视频、SRT/GT 与未分类生成文件。`--apply` 会执行实际删除；在真实工作区使用前必须先取得
用户的再次明确授权。它从不清理分支、worktree、模型、外部资源或固定本地媒体。

### 原生 C++ 构建（vision/mock 产品路径，**不强制 uv**）

```bash
cmake -S cpp -B build/cpp -G Ninja -DCMAKE_BUILD_TYPE=Release -DSUBLIFT_ENABLE_VISION=ON
cmake --build build/cpp
# 产物：build/cpp/bin/sublift（与 sublift_cli）+ sublift_worker
```

> Vision 未安装时，OCR 集成测试自动跳过，pipeline 可用 MockOcrEngine 跑闭环测试。
> PaddleOCR 首次运行会下载模型到 `~/.cache/sublift/rapidocr-models`（需联网），之后可离线推理。可预先下载：

```bash
uv run --extra paddle python -c "from sublift.ocr import PaddleOcrEngine; PaddleOcrEngine()"
```

> 若 PaddleOCR 初始化失败，CLI 会显示失败原因、网络重试提示和上述预下载命令，不会输出 Python traceback。

### Paddle Native Release 构建

Paddle 产品 Candidate 必须使用与 Python Oracle 相同、已通过性能门的官方 ORT
二进制，不能只凭相同版本号替换成 Homebrew dylib：

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
`@loader_path/../lib`，开发态运行不依赖虚拟环境内的 ABI 软链。完整 `.app` 内置模型、
签名、公证按 ADR-0030 后置，不属于当前开发构建。

## 使用

### 原生 C++ CLI（支持 vision / paddle / mock 全引擎，无需 uv）

```bash
./build/cpp/bin/sublift extract <video> -o output.srt
./build/cpp/bin/sublift extract clip.mkv --engine paddle -o out.srt
./build/cpp/bin/sublift extract clip.mkv --fps 5 --script cjk -o out.srt
./build/cpp/bin/sublift extract clip.mkv --engine mock -o out.srt
```

### 产品 CLI / Oracle / 回滚

```bash
uv run sublift extract <video> -o output.srt                 # 默认 runtime=cpp → spawn C++ worker
uv run sublift extract clip.mkv --runtime python -o out.srt  # 强制 Python worker
uv run sublift extract clip.mkv --engine paddle -o out.srt                   # C++ Paddle（可用时的产品默认）
uv run sublift extract clip.mkv --engine paddle --runtime python -o out.srt  # Python Paddle 回滚 / Oracle
SUBLIFT_RUNTIME=python uv run sublift extract clip.mkv -o out.srt  # 一键回滚
```

Phase 6.8 最终门已通过：120s canonical 上 C++ wall median 为 Python 的
`0.8956x`、进程树 RSS 为 `0.9152x`；3 来源 614.272s 的质量输出逐源 SHA exact。
C++ Paddle 不可用时返回明确错误，不会静默改成 Python、Vision 或 Mock。需要
Python Oracle/开发回滚时必须显式指定 `--runtime python` 或 `SUBLIFT_RUNTIME=python`。

### Runtime 矩阵

| engine | 产品默认 runtime | 说明 |
|---|---|---|
| **vision** | **cpp** (`sublift_worker`) | macOS 主路径 |
| **mock** | **cpp** | CI / 流程验证 |
| **paddle** | **cpp**（可用时）；不可用则报错 | 完整 DB/Quad/Cls/Rec Native；显式 runtime=python 仅用于 Oracle/开发回滚 |

解析优先级：**显式 `--runtime` / GUI 覆盖** → **`SUBLIFT_RUNTIME=python|cpp`** → 当前产品自动策略。

### 一键回滚

```bash
export SUBLIFT_RUNTIME=python
# CLI 与 GUI 均恢复 Python worker；Python 树保留，不删
```

### CLI 参数

| 参数 | 默认值 | 说明 |
|---|---|---|
| `<video>` | 必填 | 输入视频路径 |
| `-o, --output` | output.srt | 输出字幕文件路径 |
| `--fps` | 5.0 | 帧采样率（推荐 5.0） |
| `--confidence` | 0.5 | OCR 高置信门；低置信文本仅在多帧共识等条件满足时放行 |
| `--engine` | vision | OCR 引擎（vision / paddle / mock）；Paddle 首次运行需下载模型 |
| `--runtime` | 自动（cpp） | `python` \| `cpp`；覆盖 env 与自动策略；`python` 是 Paddle 的保留回滚路径 |
| `--script` | auto | 字幕文字系统（auto / cjk / latin）；已知字幕语言时可显式指定 |

## macOS GUI（开发者构建）

> Phase 2 GUI 当前通过 SwiftPM 构建运行，**不做独立 `.app` 分发包**（见 ADR-0009）。
> GUI 默认启动 C++ `sublift_worker`；只有显式设置 `SUBLIFT_RUNTIME=python` 时才使用
> Python Worker 作为 Oracle / 开发回滚。

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
- **字幕编辑**：双击文本修改、合并/拆分条目
- **引擎切换**：工具栏/设置中切换 vision / paddle / mock
- **SRT 导出**：点击「导出 SRT」选择保存路径

> 处理 mkv 需要系统已安装 ffmpeg，否则 UI 会提示 `brew install ffmpeg`。
>
> Phase 10 已完成 Native Workbench 设计与 Feature 拆解，但产品界面尚未升级；
> 实施真源见 [UI 设计入口](docs/design_ui/README.md)。

## 开发

```bash
./init.sh                     # 开工基线，会话开始/结束时运行
./scripts/verify-standard.sh  # 标准产品验证
uv run pytest                 # Python 单元测试
uv run pytest -m integration  # 集成测试（需外部视频、ffmpeg、Vision 或 Paddle 模型）
uv run ruff check .           # lint
uv run mypy src tests         # 类型检查（strict）
```

### Harness 与进度

`phases.json → docs/phases/phase*.json` 是当前开发 Phase 和 feature 的操作真源；
`.agent/` 当前只保留轻量规则、会话入口和提交辅助，复杂能力编排已从活跃 Harness 移出，
后续按真实需要增量引入。根 `feature-list.json` 仍保留给历史文档和旧工具兼容，不能用于
选择新任务。详见 [AGENTS.md](AGENTS.md) 与 [ADR-0033](docs/DECISIONS.md)。

### 架构与设计

- [架构设计](docs/ARCHITECTURE.md) — 模块布局、数据流、分层原则
- [需求规格](docs/REQUIREMENTS.md) — 功能需求、非功能需求、验收标准
- 设计文档：[UI vNext](docs/design_ui/README.md) · [pipeline](docs/design/pipeline.md) · [ocr](docs/design/ocr.md) · [extractor](docs/design/extractor.md) · [benchmark](docs/design/benchmark.md) · [macos-gui 当前实现](docs/design/macos-gui.md) · [OCR 内部性能归因（Phase 4.2 计划）](docs/design/ocr-performance-attribution.md)
- [Benchmark 用法](benchmark/README.md) — 统一 `run/matrix/score` 入口、产物与回归锚点
- [已知障碍](docs/HURDLES.md) — 开发中遇到的技术问题与解决方案

### Phase 3 固定 GT 水位

Zootopia 固定片段（1080p、5fps、统一 diagnostic 口径）的最终结果：timing recall 96.6%、precision 98.8%、F1 97.7%，CER macro 3.2%（字符准确率 97.6%），usable subtitle recall 92.0%，空文本与噪声均为 0。该结果用于回归锚点，不代表对其他片源的泛化保证；非 Zootopia 长视频 GUI 手工体验验收已在 Phase 4 完成，但英文/中英混排/不同字幕位置的量化 GT 扩充仍是后续工作。

Benchmark 统一使用 `uv run sublift-benchmark`；入口与指标说明见
[benchmark/README.md](benchmark/README.md)（设计见
[docs/design/benchmark.md](docs/design/benchmark.md)）。已验收的
[质量与性能归因基线](benchmark/baselines/README.md)随仓库版本化。版本化配置引用的固定
本地媒体保留在 `debug/` 根目录且不入库；GUI/C++ 导入、运行、性能报告与历史归档统一写入
`debug/benchmark/`。

### 技术栈

Python 3.12+ / Swift 5.9+ / uv / SwiftPM / Pillow / NumPy / OpenCV / PyObjC（Vision+Quartz）/ rapidocr（PaddleOCR）/ ffmpeg
