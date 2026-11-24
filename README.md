# SubLift

**SubLift** 是一款高性能、离线优先的视频硬字幕提取、时间轴精准对齐与校对工作台，由 **C++20 算法核**、**Tauri 2 (Rust) 原生桌面宿主** 与 **React 19 校对工作台** 构成，支持命令行批处理与桌面端一体化作业。

[![License](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C.svg)
![Tauri 2](https://img.shields.io/badge/Tauri-2-FFC131.svg)

---

## 核心特性

- **多引擎硬字幕提取（首尾同源）**
  - **本地 PaddleOCR（ONNX Runtime）**：无需 Python 运行时，本地离线高精度识别中/英及混合硬字幕。
  - **Apple Vision（macOS 原生）**：通过 ObjC++ 适配器直接调用系统 Vision 框架，零模型部署即可运行。
  - **多模态大模型（VLM）**：内置 Rust 原生流式拼图管线，支持 OpenAI 兼容端点（MiniMax、NVIDIA NIM、自定义网关），具备并发限流退避、尾延迟对冲、四级二分自救与断点续传；同时提供独立 Python 评测工具 `tools/vlm_ocr`。
- **高精度自动选区与纯毫秒打轴**
  - 自动探测视频硬字幕带（`--auto-region`）或交互式框选 ROI，支持宽字幕条自适应掩码与 64 位 dHash + 掩码 SSIM 变化点状态机。
  - 所有引擎共用同一套 C++ 前置打轴与后置 4-Pass 去重合并及 SRT 序列化，杜绝不同引擎的时间轴漂移。
- **纯桌面原生校对工作台**
  - 零本地端口监听，前端与宿主全量走 Tauri IPC 与自定义 `sublift://` 媒体流协议（原生 Range 直传 + MKV 实时 Remux 预览）。
  - 统一单/多集队列、参数批量套用、高密度虚拟列表校对器（编辑 / 合并 / 拆分 / 查找替换 / Undo-Redo）以及低置信度局部 VLM 重识。
- **严格的跨栈分层架构**
  - C++ 算法核划分为 `core` / `features` / `timing` / `text` / `ports` / `engine` / `adapters` 单向分层，核心头文件零第三方类型泄漏，并配有自动化依赖治理脚本（`scripts/check_dependencies.py`）。

---

## 系统架构概览

```text
 桌面端 (desktop/) — Tauri 2 + React 19 + TypeScript + Rust (Tokio)
   ├── src/        ui/ → store/ → core/   (单向分层视图与纯函数领域层)
   └── src-tauri/  commands / media / storage / vlm / worker
                         │
                         │ stdio JSON-Lines (本地常驻进程，零网络端口)
                         ▼
 组合根 (tools/) — sublift_worker (桌面守护) · sublift_cli (命令行)
                         │
                         ▼
 C++20 算法核 (cpp/)
   ├── sublift_engine    流水线编排 · 自动选区 · 并发与背压
   ├── sublift_timing    纯毫秒变化点状态机 · 时间轴与代表帧收集 (私有消费 features)
   ├── sublift_features  自适应掩码 · dHash · SSIM (唯一私有依赖 OpenCV)
   ├── sublift_text      行评分 · 多帧共识 · 4-Pass 去重合并 · SRT 序列化
   ├── sublift_ports     IFrameSource / IOcrEngine / ISubtitleSink / IDetector
   ├── sublift_core      强类型坐标 · ImageView / ImageBuffer · 领域模型
   └── adapters/         ffmpeg (子进程) · paddle (ORT) · apple_vision (macOS) · mock
```

更完整的模块边界与协议规范请查阅：
- [算法核架构设计文档](docs/ARCHITECTURE.md)
- [桌面前端与宿主架构文档](docs/DESKTOP_ARCHITECTURE.md)

---

## 环境要求

- **操作系统**：macOS 13.3+（Apple Silicon / x86_64，支持 Apple Vision）或现代 Linux
- **C++ 工具链**：支持 C++20 的编译器（Apple Clang >= 15 / Clang >= 16 / GCC >= 12）、CMake >= 3.20、Ninja >= 1.10
- **核心依赖**：
  - `ffmpeg` 与 `ffprobe`（位于 `PATH` 中，用于视频解码与探测）
  - OpenCV >= 4.0（`core`、`imgproc`、`imgcodecs`）
  - ONNX Runtime（使用 PaddleOCR 引擎时需要；模型与库校验清单见 [resources/manifest.json](resources/manifest.json)）
  - Catch2 v3.7.1 与 nlohmann/json v3.11.3（未在系统找到时由 CMake `FetchContent` 自动拉取）
- **桌面端开发额外依赖**（仅构建 `desktop/` 时需要）：Node.js >= 20、Rust >= 1.90
- **脚本与检查**：Python >= 3.9

---

## 快速开始

### 1. 构建 C++ 核心、CLI 与 Worker

所有构建目录统一放在 [`build/`](build/README.md) 子目录下：

```bash
# Debug 构建（日常开发与测试）
cmake -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug

# Release 构建（性能运行与桌面端配套）
cmake -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
```

> 在非 macOS 平台上 CMake 会自动关闭 Apple Vision 适配器；在 macOS 上可通过 `-DSUBLIFT_ENABLE_VISION=OFF` 手动关闭。

### 2. 使用命令行工具（`sublift_cli`）

```bash
# 自动探测字幕区域并提取 SRT（PaddleOCR）
./build/release/tools/cli/sublift_cli video.mp4 -o result.srt --auto-region --fps 5.0

# 手动指定 ROI（x,y,w,h）
./build/release/tools/cli/sublift_cli video.mp4 -o result.srt --region 0,778,1920,121

# 纯打轴模式（跳过 OCR，输出时间段 JSONL / SRT）
./build/release/tools/cli/sublift_cli timing video.mp4 -o timing.jsonl --auto-region

# 查看全部参数
./build/release/tools/cli/sublift_cli --help
```

### 3. 运行桌面工作台（Tauri 2）

先完成 C++ 构建（确保 `sublift_worker` 已生成），然后启动桌面开发态：

```bash
cd desktop
npm install
npm run tauri dev
```

桌面端运行时不会向用户视频目录写入缓存；VLM 供应商配置（`0600` 权限）、校对草稿与续传断点均保存在系统 `AppData` 私有目录中。

### 4. 独立多模态 VLM 离线工具（可选）

如需通过命令行将 `sublift_cli --timing-only` 结合 VLM 端点进行批量识别或算法对标，参考 [tools/vlm_ocr/README.md](tools/vlm_ocr/README.md) 与 [.env.example](.env.example)。

---

## 测试与架构检查

```bash
# 1. 跨栈架构分层依赖静态检查
python3 scripts/check_dependencies.py

# 2. C++ 单元测试（纯内存与 Mock，无需真实视频/模型）
ctest --test-dir build/debug -L unit --output-on-failure

# 3. C++ 全量测试（unit + parity 黄金基线 + adapter + 短片段 e2e）
ctest --test-dir build/debug --output-on-failure

# 4. 桌面端测试（前端 Vitest + Rust Cargo Test）
npm --prefix desktop test
cargo test --manifest-path desktop/src-tauri/Cargo.toml
```

---

## 许可证与第三方声明

本项目源码采用 [Apache License 2.0](LICENSE) 开源许可。

- **Copyright 2026 Hudea**
- 本项目通过外部独立进程调用 `ffmpeg` / `ffprobe`，不静态或动态链接 FFmpeg 库；若自行打包或分发第三方二进制组件（如 FFmpeg、ONNX Runtime、OpenCV 或 PaddleOCR 模型），请同时遵守各组件自身的开源许可证条款。
