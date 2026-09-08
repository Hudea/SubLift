# 开发说明

面向在本仓库改代码、跑验证的人。日常使用见根目录 [README](../README.md)。

## 环境

- macOS 13+（GUI 与 Vision；Apple Silicon 推荐）
- ffmpeg（含 ffprobe）
- CMake 3.20+ 与 C++20 工具链（推荐 Ninja）
- Xcode 15+ 或 SwiftPM（仅 GUI）
- Node.js 20+（仅 Web UI）
- Docker Engine 24+（仅局域网容器验收；默认产品门不依赖 Docker）

Python 3.12+ 与 `uv` 只服务隔离的可选离线工具（benchmark、评分、冻结 Oracle），
不是产品安装条件，也不得成为 Native 产品运行或产品门的依赖。

## 开工与验证

```bash
./init.sh                     # 开工基线，会话开始/结束时运行
cmake --build build/cpp
ctest --test-dir build/cpp --output-on-failure
(cd apps/macos && swift test)
npm --prefix apps/web test
./scripts/verify-product.sh   # Python-free 产品门（提交/合并默认；不依赖 Docker）
./scripts/verify-container.sh # 局域网容器门（需要 Docker daemon）
./scripts/verify-offline.sh   # 隔离离线工具
./scripts/verify-standard.sh  # 显式过渡混门 / 历史 cutover；非默认
```

`./init.sh` 只建立开工前提。产品行为以 `./scripts/verify-product.sh` 为准：不安装
Python 依赖、不运行 Python 脚本，也不因为缺少 `python` / `uv` / `.venv` 或 Docker
daemon 而跳过或失败。

容器验收是独立入口 `./scripts/verify-container.sh`（需要 Docker daemon；无 daemon
时打印 SKIPPED 并以非 0 退出，不假绿）。

仅在维护隔离的 Python benchmark、诊断或历史 Oracle 时才运行：

```bash
uv sync --extra oracle --extra vision --extra paddle
uv run ruff check .
uv run mypy src tests
uv run pytest -m "not integration" --no-cov
```

## Debug 构建

```bash
cmake -S cpp -B build/cpp -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DSUBLIFT_REQUIRE_OPENCV=ON \
  -DSUBLIFT_ENABLE_VISION=ON
cmake --build build/cpp
ctest --test-dir build/cpp --output-on-failure
```

Vision 未安装时，OCR 集成测试自动跳过，pipeline 可用 MockOcrEngine 跑闭环测试。

## 本机 Web / Native Server

```bash
cmake --build build/cpp --target sublift_server
npm --prefix apps/web run build
./build/cpp/bin/sublift_server --static-dir apps/web/dist
# 浏览器打开 http://127.0.0.1:8080
```

默认只绑 loopback。非 loopback 监听必须已配置并锁定媒体根
（`SUBLIFT_MEDIA_DIR` / `--media-dir`），否则拒绝启动。

## 可选清理工具

当前本地产物清理器仍由 Python 实现，不是产品命令。默认只预览可重建产物及预计释放
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

## 进度与 Agent 契约

`phases.json` → `docs/phases/phase*.json` 是当前开发 Phase 和 Deliverable 的操作真源。
根 `feature-list.json` 只保留给历史文档和旧工具兼容，不能用于选择新工作。

会话入口与提交约定见 [AGENTS.md](../AGENTS.md)。复杂能力编排已从活跃 Harness 移出。

## 架构与设计

- [架构](ARCHITECTURE.md)
- [需求规格](REQUIREMENTS.md)
- [Native C++ 契约](cpp/README.md)
- [决策记录](DECISIONS.md)
- [已知障碍](HURDLES.md)
- [测试全景](TESTMAP.md)
- 设计：[UI vNext](design_ui/README.md) · [pipeline](design/pipeline.md) · [ocr](design/ocr.md) · [extractor](design/extractor.md) · [benchmark](design/benchmark.md) · [macos-gui](design/macos-gui.md)
- [Benchmark 用法](../benchmark/README.md)

## 质量锚点

Zootopia 固定片段（1080p、5fps）曾作为 Phase 3 回归水位：timing recall 96.6%、
precision 98.8%、F1 97.7%，CER macro 3.2%（字符准确率 97.6%），usable subtitle
recall 92.0%，空文本与噪声均为 0。该结果用于回归锚点，不代表对其他片源的泛化保证。

隔离的 Python Benchmark 入口是 `uv run sublift-benchmark`；版本化配置引用的固定本地
媒体保留在 `debug/` 且不入库。该工具可以辅助分析 Native 输出，但不得成为产品运行或
最终 Native 门禁依赖。

## 技术栈

C++20 / ObjC++ / CMake / Ninja / Swift 5.9+ / SwiftPM / Vue 3 / TypeScript / OpenCV /
ONNX Runtime / ffmpeg；Python 3.12+ 与 uv 仅用于隔离的可选离线工具。
