# SubLift 架构设计

> 本文只描述已确认的目标产品结构与依赖边界，不承载迁移计划、进度播报或验收报告；
> 实现状态以 `phases.json` 与 `progress.md` 为准。

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
- 可选离线研究工具、golden/GT 资产与诊断工具位于产品依赖图之外，不得被产品 target
  import、启动或探测。

## 3. 模块布局

| 路径 | 职责 |
|---|---|
| `cpp/` | Native 产品实现：Core、Pipeline、Application、Protocol、Adapters、Worker、CLI 与 Server |
| `apps/macos/` | SwiftUI 工作台、批量任务中心、播放与编辑状态，以及 UDS 客户端 |
| `apps/web/` | Web 工作台与批量任务中心，通过 HTTP/SSE 使用 Native Server |
| `src/sublift/` | 冻结 Oracle 与离线工具内部实现；位于产品依赖图之外，不随产品新能力演进 |
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

历史算法背景见 [pipeline](design/pipeline.md)、[OCR](design/ocr.md) 与
[extractor](design/extractor.md)；这些 Python 设计只用于迁移追溯。当前产品契约、Native 实现与
parity 边界见 [C++ 文档入口](cpp/README.md) 及其可执行测试。

## 5. 核心数据模型

C++ Core 定义产品运行时的领域模型；版本化 golden、固定 GT 与 Native tests 约束其行为。
协议 DTO、Swift 状态和 Web 类型只做边界映射，不取代领域模型。

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

产品只使用 Native C++ runtime；macOS、CLI 与 Web 采用不同宿主边界，但复用同一
Application、Pipeline、Ports 与 Adapters。

| 入口 | 运行路径 |
|---|---|
| macOS Workbench / Task Center | SwiftUI → `PipelineClient` → UDS → C++ Worker |
| Native CLI | 参数解析与协议客户端 → UDS → C++ Worker |
| Web Workbench / Task Center | Browser → HTTP/SSE → Native Server → 进程内 Application / Pipeline |

- Vision、Mock 与 Paddle 在 Native capability 可用时走 C++；能力缺失时明确报错，不静默换引擎或进入 Python。
- Worker 与 Native Server 都发布结构化进度、增量字幕、最终结果和错误身份；最终结果替换增量预览。
- 版本回滚使用上一版已验收 Native artifact、release/tag 或 Git revision，不提供同版本
  Python runtime fallback。

产品架构不定义任何 Python runtime 路径；仓库中与此冲突的兼容代码属于实现偏差，不构成
受支持边界。

### 7.1 Web / Native Server 会话与结果所有权

Web 入口仍是本机产品边界，不因使用 HTTP 而获得读取任意本机文件或丢弃任务状态的例外：

- `WorkspaceManager` 提供当前唯一媒体授权根；stream、frame、detect、resolve、scan、job
  和 export 对路径做同一套 canonical、symlink 与内部缓存边界校验。Native Server 默认只
  绑定 loopback，扩大监听范围必须显式采用独立访问控制合同。
- JobManager 暴露可查询的配置、状态、事件 cursor 与最终结果快照。Web 队列以 job id 关联，
  持久化状态版本化、原子写且有界；服务重启后活动任务变为 interrupted，不自动恢复 OCR。
- SSE 事件使用 job 内单调序号，重连从 Last-Event-ID/cursor 继续；客户端按 job id + seq
  去重。进度协议使用 `0.0–1.0`，仅在 UI 显示边界换算为百分数。
- 单视频与批量入口映射到同一配置快照，显式携带 ROI policy。最终字幕、用户审阅草稿与
  输出文件各有版本/状态；浏览器下载不等同于服务端已保存，磁盘 completed 必须以原子写
  成功为准。

### 7.2 容器自托管部署边界

Web / Native Server 可以以容器镜像部署到可信局域网服务器（ADR-0040，Phase 14）。容器化
不改变第 7.1 节的会话与结果所有权，只改变宿主的挂载与网络暴露方式：

- 拓扑为单容器同源部署：容器内 `sublift_server` 同时托管 Web 静态资源与 API；不引入反向
  代理或前端独立服务。媒体入口是宿主目录挂载到容器 `/media`，不新增上传 API。
- 非 loopback 监听必须满足最小访问控制合同：启动时必须已有媒体根；启动期注入的媒体根
  锁定工作区，`POST/DELETE /api/config/workspace` 返回 403；可选共享 token
  （`SUBLIFT_ACCESS_TOKEN`）要求 `/api/*` 携带 `Authorization: Bearer`。CORS 继续默认关闭。
- 镜像内的模型与 ONNX Runtime 由 `resources/manifest.json` 的版本与 SHA-256 约束，校验
  失败即构建失败；Paddle capability 缺失时 fail-closed，不静默降级为 mock。推理为 CPU
  ONNX Runtime；GPU/CUDA 不在部署边界内。
- 缓存、抽帧与导出落在宿主媒体目录下的 `.sublift_cache/`。更新方式是仓库内
  `docker compose build` 后 `up -d`；不推镜像仓库、不做 CI/CD。管理员操作说明见根 [README](../README.md) 的局域网 Web 一节。
- 容器验收是独立于默认产品门的入口（`./scripts/verify-container.sh`），由真实
  `docker build`、容器内 `paddle.available` 与经 `/media` 映射的 golden SRT 比对构成。
  公网 HTTPS、账号系统、镜像仓库与 CI/CD 不属于当前部署边界。

## 8. 配置、能力与资源

配置在产品入口完成解析和校验，由 Application 为每个 job 创建不可追溯修改的快照；运行中的任务不读取 UI 的后续变更。

| 主题 | 边界 |
|---|---|
| 配置 | CLI、Swift 与 Web 输入映射为统一 Native Config；未知字段、非法枚举和越界 ROI 显式拒绝 |
| Capability | 由当前二进制、平台、Adapter 和资源共同声明；UI 只展示探测结果，不自行推断 |
| 引擎选择 | 宿主根据显式配置装配 Adapter；不可用时 fail-closed，不静默改引擎 |
| 模型与资源 | `ResourceLocator` / `ModelBundle` 解析模型和运行时资源；路径查找不进入 Core 或 Pipeline |
| 媒体路径 | Worker 在宿主边界校验；Server 以 `WorkspaceManager` 的 canonical media root 统一授权和解析；领域层只接收已解析输入 |
| Web 任务状态 | 配置快照、queue/job 状态、事件 cursor 和终态结果使用工作区内的版本化、有界持久化；损坏或未知版本 fail-closed |
| Web 输出 | 浏览器下载与工作区保存分开；服务端保存必须在授权根内按冲突策略原子落盘，Review 导出使用当前草稿版本 |

参数默认值和完整字段以 Native Config 定义及其契约测试为准，本文不复制字段清单。

## 9. 平台与验证边界

| 范围 | 约束 |
|---|---|
| 跨平台 Native 层 | C++20 Core、Pipeline、Application 与 Protocol 不依赖 UI 或平台 Framework |
| macOS | SwiftUI 产品入口；Apple Vision 仅存在于私有 ObjC++ Adapter |
| Web / Server | Vue/TypeScript 前端通过标准 HTTP/SSE 使用 C++ Native Server |
| OCR 与媒体依赖 | ONNX Runtime、OpenCV、Apple Framework 与 FFmpeg 封装在对应 Adapter 或宿主内部 |
| 可选离线工具 | 如确有需要可使用 Python 等工具生成研究数据，但与产品构建、运行和 Native 日常验证隔离 |

验证按层隔离：Core/Pipeline 使用确定性单元测试，Ports 与协议使用契约测试，Adapters 使用
引擎专项测试，产品入口使用 C++/Swift/Web 与 E2E 测试；产品行为真源是版本化 golden、
固定 GT 与 Native tests。产品 target 不依赖 test support、diagnostics 或 Python runtime。

架构细节见 [C++ 文档](cpp/README.md) 与 [模块设计](design/)；需求边界见
[`REQUIREMENTS.md`](REQUIREMENTS.md)，历史决策见 [`DECISIONS.md`](DECISIONS.md)，当前进度只以
[`phases.json`](../phases.json) 及其 `detail_file` 为准。
