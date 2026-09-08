# Changelog

All notable changes to this project are documented in this file.

## [Unreleased]

## [0.1.0] — 2026-09-08

第一版技术预览：本地硬字幕提取，输出 SRT。源码按 MIT 许可。

### 可用

- **Native CLI**：`sublift extract` 从视频画面识别烧录字幕并写出 SRT；能力不足时明确失败，不切换 Python 或其它引擎。
- **macOS GUI**：SwiftUI 工作台，拖拽导入、预览、区域选择、增量结果、校对与 SRT 导出；批量任务中心（⌘⇧T）。通过 `swift run` 在开发者环境运行，不是签名 `.app`。
- **Web / Native Server**：本机 loopback 工作台；Linux 局域网可用 Docker Compose 自托管（CPU Paddle，宿主目录挂载为唯一媒体根）。
- **引擎**：macOS 上 Apple Vision（默认中英）与 PaddleOCR（PP-OCRv6 + ONNX Runtime）；Linux 容器为 CPU Paddle。模型按 `resources/manifest.json` 校验安装。
- **离线**：视频与识别文本不上传；模型齐备后不依赖网络。

### 不包含

- GPU / CUDA
- 已签名、已公证的 macOS 安装包
- 公网 HTTPS、账号、反向代理、镜像仓库自动发布
- ASS / VTT 导出
- 软字幕轨提取、翻译、云端服务
- Python 产品运行时（Python 仅保留为隔离的可选离线工具）

### 使用注意

- 局域网部署仅限可信网络；不要把端口暴露到公网。
- 不要为浏览器工作台启用 `SUBLIFT_ACCESS_TOKEN`（网页不会带 Bearer，会 401）。该 token 只给脚本 / API。
- 处理 mkv 需要系统 ffmpeg。

## 开发历史

以下条目保留开发期记录，不是 0.1.0 的用户发布说明。

### [Unreleased] — Phase 13 D06: isolate offline tools

- Benchmark default extract backend is Native CLI. Frozen Python Oracle is `--backend oracle` / `"backend": "oracle"` only.
- Scoring, matrix, and report imports do not load Pipeline, OCR, OpenCV, or Pillow.
- Optional Python tools use the `sublift_offline` namespace, empty default deps plus `oracle`/`vision`/`paddle` extras, and `./scripts/verify-offline.sh`. They do not occupy the product `sublift` command.
- Tool outputs stay under `debug/benchmark/`. Python Oracle is frozen and does not track new Native features.

### [Unreleased] — Phase 13 D05: remove Python product implementation

- Removed Python product CLI, IPC server/bridge, runtime resolver, and the `sublift` console script.
- Remaining `src/sublift` is isolated offline tools (benchmark / frozen Oracle). Product extract is Native only.
- Last runnable Python product revision is Git tag `python-product-last`.

### [Unreleased] — Phase 13 D04: Python-free product gate

- Product verification is `./scripts/verify-product.sh`. It does not install or run Python, and it does not skip required checks when python/uv/.venv are missing.
- The gate runs CTest, Swift tests, Web Vitest/build, Native resource/ORT/capability probes, CLI mock and paddle extract, and a Native Server job that exports SRT.
- `./scripts/verify-standard.sh` remains the transitional Oracle/parity gate.

### [Unreleased] — Phase 13 D03: Native model and ORT resource loop

- Paddle PP-OCRv6-small files and ONNX Runtime are pinned by `resources/manifest.json` (SHA-256).
- `sublift resources install` writes a temporary file, verifies the digest, then atomically replaces the destination. A failed fetch is not a usable bundle.
- CMake and `ResourceLocator` reject ONNX Runtime from `.venv` / `site-packages`. macOS Homebrew 1.28.0 and official GitHub linux/darwin archives are the allowed sources.
- Default model cache is `~/.cache/sublift/models/ppocrv6-small`. SHA-matching files in the historical RapidOCR cache may still be reused; new installs do not write there.

### [Unreleased] — Phase 13 D02: close dual product runtime

- Product CLI is Native `build/cpp/bin/sublift`. `--runtime` and `SUBLIFT_RUNTIME=python` fail closed; they no longer select a Python implementation.
- macOS starts only `sublift_worker`. It does not launch `python -m sublift.ipc.server`, inject `PYTHONPATH`, or treat `.venv` as a product locator.
- Missing Native worker, engine, or capability returns a structured error. There is no silent engine switch and no Python fallback.
- The Python `sublift extract` console entry prints a Native CLI redirect and exits 2. Isolated Oracle/benchmark tools remain until later Phase 13 deliverables.

### [6.6] — 2026-07-28 — Cutover: default C++ runtime for vision/mock

#### Product default

- **vision / mock** product path now defaults to the **C++** `sublift_worker` (CLI + macOS GUI).
- **paddle** always routes to the **Python** IPC worker (no silent fallback to vision/mock).
- Priority: explicit CLI/GUI flag → `SUBLIFT_RUNTIME` env → product default (`cpp`).

#### Native CLI (no uv required for vision/mock)

- Build artifacts: `build/cpp/bin/sublift` (and `sublift_cli`) — `extract <video> [options]`.
- Spawns sibling `sublift_worker` over UDS; writes SRT.
- `uv run sublift` remains the **oracle / paddle / rollback** Python entry.

#### Rollback (one-key)

```bash
export SUBLIFT_RUNTIME=python   # CLI + GUI resolve to Python worker
# or per-invocation:
uv run sublift extract <video> --runtime python -o out.srt
```

Python tree is **not** deleted; oracle, benchmark, and paddle keep working.

#### Quality / performance (cutover gate)

- Correctness: 10 parity goldens (`scripts/parity/check_cutover_gate.py --check`).
- Runtime (mock path-mode): cancel ≤1s (hard), restart ≤5s (hard), wall ≤×1.30+0.05s, RSS ≤×1.50.
- GT L3: fixed Zootopia waterline wired; live measurement when `debug/Zootopia_clip_1080p.mp4` is present; otherwise **WAIVED** per ADR-0022 (report must not claim full publish-contract).
- Evidence: `docs/phases/phase6.json` (Phase 6.6 cutover entries).

#### Known limits

- Paddle remains Python-only until a native adapter exists.
- Limited GT inventory (single fixed clip, not vendored).
- Merged residual / timing edge cases tracked in progress.
