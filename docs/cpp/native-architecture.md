# Native C++ 架构

## 1. Target 与职责

| Target | 职责 | 主要依赖 |
|---|---|---|
| `sublift_core` | 领域模型、Config、图像/坐标、错误模型与纯算法 | C++20；图像实现依赖仅可私有 |
| `sublift_models` | `ResourceLocator`、`ModelBundle` 与模型资源解析 | core |
| `sublift_pipeline` | signature、changepoint、timeline、line select、dedupe 与流式 Pipeline | core；OpenCV 私有 |
| `sublift_protocol` | IPC DTO、JSON 映射与 framing | core；nlohmann/json 私有 |
| `sublift_application` | Bridge、提取用例、任务生命周期、进度、取消与错误映射 | pipeline、protocol、core |
| `sublift_ffmpeg` | 媒体探测、full/ROI 抽帧与子进程控制 | core；models 私有 |
| `sublift_vision_macos` | Apple Vision OCR Adapter | core；Apple Framework 私有 |
| `sublift_paddle` | Paddle Det/Cls/Rec Adapter | core；OpenCV/ONNX Runtime 私有 |
| `sublift_worker_runtime` | OCR/Detector/媒体服务工厂与 UDS session | application、protocol；Adapters 私有 |
| `sublift_cli` | 参数解析、Worker 启动、IPC 客户端与 SRT 输出 | protocol、core |
| `sublift_server_runtime` | HTTP/SSE、JobManager、媒体沙箱与 Web 宿主 | application、worker runtime、Adapters 与 server 依赖 |
| `sublift_test_support` / diagnostics | Golden、fixture、比较器与 Paddle trace | 仅测试和显式诊断 |

## 2. 依赖方向

```text
sublift_cli ───────────────► sublift_protocol
                                  │
sublift_worker ─► worker_runtime ─┼─► sublift_application ─► sublift_pipeline ─► sublift_core
                     │            │
                     └─► FFmpeg / Vision / Paddle Adapters ────────────────► Ports

sublift_server ─► server_runtime ─► application + worker runtime + Adapters
```

Ports 位于 `cpp/include/sublift/ports/` 与 Application 的 factory/service 接口中。Pipeline 和
Application 只依赖 SubLift 自有模型及抽象能力；Worker 与 Server 作为宿主装配具体实现。

## 3. 目录所有权

| 路径 | 所有权 |
|---|---|
| `cpp/include/sublift/core/`、`cpp/src/core/` | 领域模型和纯算法 |
| `cpp/include/sublift/pipeline/`、`cpp/src/pipeline/` | 流式字幕处理 |
| `cpp/include/sublift/application/`、`cpp/src/application/` | 产品用例与 Bridge |
| `cpp/include/sublift/protocol/`、`cpp/src/protocol/` | UDS DTO 与 framing |
| `cpp/include/sublift/adapters/`、`cpp/src/adapters/` | FFmpeg、Vision、Paddle 具体实现 |
| `cpp/src/worker/` | Worker composition root 与 UDS connection |
| `cpp/include/sublift/server/`、`cpp/src/server/` | Native Web Server |
| `cpp/src/cli/` | Native CLI |
| `cpp/src/test_support/`、`cpp/diagnostics/` | 非产品验证与诊断 |

根级兼容头只做转发，不承载新实现；新公共接口应落在对应职责目录。

## 4. 核心约束

- 公共 API 不暴露 `cv::Mat`、Ort、Objective-C、nlohmann/json 或 Pillow 类型。
- `ImageBuffer` 拥有像素内存；`ImageView` 只读借用且不得超过底层 Buffer 生命周期。
- `SourceBox`、`FrameLocalBox` 与 `OcrCropBox` 不得隐式混用。
- 单个 Pipeline 和 OCR 实例串行消费；每个 job 独占其 Pipeline、Extractor 与取消状态。
- Application 不构造具体 Adapter；宿主通过 factory/service 注入。
- Worker 负责 UDS 进程边界；Server 在进程内驱动同一 Application/Pipeline 语义。
- OpenCV、ORT、Vision 或模型缺失只能影响 capability，不得触发静默换引擎。

产品路由见 [Runtime 契约](runtime-contract.md)，UDS 行为见
[Worker IPC 契约](worker-ipc-contract.md)。
