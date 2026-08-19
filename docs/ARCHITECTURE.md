# SubLift 架构设计

> **Agent 维护契约**：调整本文档须先提交变更分析报告并获得用户明确授权。

## 1. 设计原则

- **模块化、可插拔、低耦合高内聚**：各能力模块定义 Protocol，默认实现可替换
- **平台 API 隔离**：平台特定 API（例如：Apple Vision、Windows Media Foundation 等）应被抽象在对应的 Protocol 实现中，核心层只依赖 Protocol
- **串联层不实现单一能力**：pipeline / export 组合能力模块，处理流程逻辑

## 2. 分层架构

SubLift 按职责分层，产品依赖向领域核心收敛；平台能力和第三方实现通过 Ports 注入。

| 层 | 职责 |
|---|---|
| 产品入口 | macOS、Web 与 Native CLI；接收用户操作并呈现任务状态和结果 |
| 宿主与传输 | Worker、Native Server 与协议 DTO；处理 UDS、HTTP/SSE、序列化和对象装配 |
| Application | 提取用例、配置快照、任务生命周期、进度、取消与错误映射 |
| Pipeline | signature、changepoint、timeline、line select、dedupe 与流式编排 |
| Core / Domain | 自有数据模型、坐标与图像契约、稳定错误模型和纯领域算法 |
| Ports / Adapters | Ports 定义抽帧、检测、OCR 等能力接口；Adapters 封装 FFmpeg、Vision、Paddle/ORT 与平台资源 |

```text
产品入口 → 宿主与传输 → Application → Pipeline → Core / Domain
                           │             │
                           └────依赖 Ports◄──── Adapters 实现
```

- Core、Pipeline 与 Application 不依赖 UI、传输协议或具体 Adapter；第三方类型不得泄漏到公共接口。
- Worker 与 Native Server 只负责协议适配和对象装配，不复制 Pipeline 算法。
- Python Oracle、benchmark、golden/parity 与诊断工具位于产品依赖图之外。

## 3. 模块布局

| 路径 | 职责 |
|---|---|
| `cpp/` | Native 产品实现：Core、Pipeline、Application、Protocol、Adapters、Worker、CLI 与 Server |
| `apps/macos/` | SwiftUI 工作台、批量任务中心、播放与编辑状态，以及 UDS 客户端 |
| `apps/web/` | Web 工作台与批量任务中心，通过 HTTP/SSE 使用 Native Server |
| `src/sublift/` | Python Oracle、benchmark 运行时、显式回滚与 IPC 兼容实现 |
| `benchmark/` | 版本化配置、数据集、质量基线、golden 与 parity 资产 |

各目录内部按第二章的职责边界继续拆分；详细文件归属由对应目录的 README 与构建配置维护，本文不复制完整文件树。

## 4. 核心处理数据流

macOS、Web 与 CLI 共享同一套 Native 字幕处理语义；入口协议不同，不改变核心处理顺序。

```text
视频 + 配置 / 可选 ROI
  → 媒体探测、抽帧与 ROI 坐标转换
  → Frame / 字幕区域图像
  → 帧签名与变化检测
  → 字幕段构建与代表帧选择
  → OCR Adapter
  → 行选择与跨帧文本共识
  → 去重合并
  → SubtitleEntry 增量结果
  → 最终 entries → 编辑 / SRT 导出
```

- ROI 在抽帧边界从 source-frame 坐标转换；Pipeline 只消费当前图像的局部坐标。
- OCR 引擎由宿主装配并通过 Port 注入，Pipeline 不选择或实例化具体 Adapter。
- `push_entry` 用于增量呈现，最终 `entries` 是编辑与导出的权威结果。

算法语义见 [pipeline](design/pipeline.md)、[OCR](design/ocr.md) 与
[extractor](design/extractor.md) 设计；Native 实现及 parity 契约见 [C++ 文档入口](cpp/README.md)。

## 5. 核心数据模型

C++ Core 定义产品运行时的领域模型；Python 使用等价模型承担 Oracle 与 parity 对照。协议 DTO、Swift 状态和 Web 类型只做边界映射，不取代领域模型。

| 模型 | 语义 |
|---|---|
| `ImageBuffer` / `ImageView` | 拥有像素内存的缓冲区与只读非拥有视图；显式携带尺寸、stride 和像素格式 |
| `SourceBox` / `FrameLocalBox` / `OcrCropBox` | 分别表示原始视频帧、抽帧输出图和 OCR 输入图的坐标空间 |
| `Frame` / `Region` | 带毫秒时间戳的采样帧，以及当前 Frame 局部坐标中的字幕区域 |
| `OcrLine` / `OcrResult` | OCR 行级文字、置信度和几何信息，以及一次识别的汇总结果 |
| `SubtitleProfile` | 字幕轨的文字系统与空间画像，供行选择和跨帧共识使用 |
| `SubtitleEntry` | Pipeline 的标准字幕结果，包含起止时间、文本和置信度 |

- 时间统一使用毫秒语义和 64 位整数；像素几何使用显式坐标空间和固定宽度整数。
- 公共模型不暴露 `cv::Mat`、Ort、Objective-C、Pillow 或协议 JSON 类型。
- `ImageView` 不拥有内存且不得超过底层 Buffer 生命周期；需要独立所有权时显式复制。

## 6. Ports 与边界契约

Ports 用 SubLift 自有模型描述外部能力，Application 与 Pipeline 依赖接口，具体实现由宿主注入。

| Port | 契约 |
|---|---|
| `IExtractor` | 暴露抽帧取消与当前 source-frame crop，是具体 Extractor 的最小公共边界 |
| `IStreamingExtractor` / `IPathMediaServices` | 探测媒体、规划 full/ROI 输出并以回调流式产生 Frame |
| `IDetector` | 在 Frame 局部坐标中解析字幕 Region；未解析时显式返回空结果 |
| `IOcrEngine` | 借用 `ImageView` 完成一次识别并返回 `OcrResult`；不得持有输入视图 |
| `IDetectorFactory` / `IOcrEngineFactory` | 按 capability 创建 Detector 与 OCR，不让 Application 知道具体 Adapter |

- FFmpeg、Vision 与 Paddle/ORT Adapter 只实现 Port，不依赖 Worker、Server 或协议层。
- 协议层负责领域模型与 UDS/HTTP DTO 的显式映射，未知消息、未知引擎和缺失 capability 必须 fail-closed。
- 单个 OCR 引擎实例按契约串行调用；取消、资源回收和 job 生命周期由 Application 管理。

## 7. 产品运行时拓扑

默认产品路径使用 Native C++；macOS、CLI 与 Web 采用不同宿主边界，但复用同一 Application、Pipeline、Ports 与 Adapters。

| 入口 | 运行路径 |
|---|---|
| macOS Workbench / Task Center | SwiftUI → `PipelineClient` → UDS → C++ Worker |
| Native CLI | 参数解析与协议客户端 → UDS → C++ Worker |
| Web Workbench / Task Center | Browser → HTTP/SSE → Native Server → 进程内 Application / Pipeline |
| 显式 Python | macOS/CLI 仅在 `runtime=python` 或 `SUBLIFT_RUNTIME=python` 时启动 Python IPC runtime |

- Vision、Mock 与 Paddle 在 Native capability 可用时走 C++；能力缺失时明确报错，不静默换引擎或回退 Python。
- Worker 与 Native Server 都发布结构化进度、增量字幕、最终结果和错误身份；最终结果替换增量预览。
- Python 路径仅用于 Oracle、benchmark 和开发回滚，不是默认产品依赖。

## 8. 配置、能力与资源

配置在产品入口完成解析和校验，由 Application 为每个 job 创建不可追溯修改的快照；运行中的任务不读取 UI 的后续变更。

| 主题 | 边界 |
|---|---|
| 配置 | CLI、Swift 与 Web 输入映射为统一 Native Config；未知字段、非法枚举和越界 ROI 显式拒绝 |
| Capability | 由当前二进制、平台、Adapter 和资源共同声明；UI 只展示探测结果，不自行推断 |
| 引擎选择 | 宿主根据显式配置装配 Adapter；不可用时 fail-closed，不静默改引擎 |
| 模型与资源 | `ResourceLocator` / `ModelBundle` 解析模型和运行时资源；路径查找不进入 Core 或 Pipeline |
| 媒体路径 | Worker、Server 在宿主边界校验和解析；领域层只接收已解析的任务输入 |

参数默认值和完整字段以 Native Config 定义及其契约测试为准；Python Config 保持 Oracle 对照，不在本文复制字段清单。

## 9. 平台与验证边界

| 范围 | 约束 |
|---|---|
| 跨平台 Native 层 | C++20 Core、Pipeline、Application 与 Protocol 不依赖 UI 或平台 Framework |
| macOS | SwiftUI 产品入口；Apple Vision 仅存在于私有 ObjC++ Adapter |
| Web / Server | Vue/TypeScript 前端通过标准 HTTP/SSE 使用 C++ Native Server |
| OCR 与媒体依赖 | ONNX Runtime、OpenCV、Apple Framework 与 FFmpeg 封装在对应 Adapter 或宿主内部 |
| Python | Python 3.12+、RapidOCR/PyObjC 等只服务 Oracle、benchmark、显式回滚和开发验证 |

验证按层隔离：Core/Pipeline 使用确定性单元测试，Ports 与协议使用契约测试，Adapters 使用引擎专项测试，产品入口使用 C++/Swift/Web 与 E2E 测试；跨实现正确性由冻结 Oracle、golden、GT 与 parity 门约束。产品 target 不依赖 test support、diagnostics 或在线 Python Oracle。

架构细节见 [C++ 文档](cpp/README.md) 与 [模块设计](design/)；需求边界见
[`REQUIREMENTS.md`](REQUIREMENTS.md)，历史决策见 [`DECISIONS.md`](DECISIONS.md)，当前进度只以
[`phases.json`](../phases.json) 及其 `detail_file` 为准。
