# SubLift 算法核架构设计与工程规范文档

本文档定义 `SubLift` 算法核的目标架构、分层拓扑、模块职责、接口契约及依赖治理规则。本文档仅描述系统的正式目标架构。

---

## 1. 架构全景与五层拓扑（Layer Topology）

`SubLift` 采用严格单向流动、依赖倒置的分层架构，将原单体管线彻底解耦为职责单一的 5 层体系。

```text
 宿主层 (Hosts / Composition Roots)
   tools/cli, tools/bench, server/worker
   │  【唯一允许同时装配 Engine 与 Adapters 的组合根】
   ▼
 ╔═════════════════════════ 算法核 (Algorithm Core) ═════════════════════════╗
 ║                                                                         ║
 ║  4. 编排层 (Engine Orchestrator)                                         ║
 ║     sublift_engine: ExtractionSession, ExtractionConfig, Observer       ║
 ║        │                    │                                           ║
 ║        ▼                    ▼                                           ║
 ║  2. 领域算法层 (Domain Pipeline Algorithms)                              ║
 ║     ┌────────────────┐    ┌─────────────────┐                           ║
 ║     │ sublift_timing │    │  sublift_text   │   (timing 与 text 互不包含)║
 ║     └───────┬────────┘    └────────┬────────┘                           ║
 ║             │                      │                                    ║
 ║             ▼                      │                                    ║
 ║     ┌────────────────┐             │                                    ║
 ║     │sublift_features│             │   (features 仅被 timing 依赖)       ║
 ║     └───────┬────────┘             │                                    ║
 ║             │                      │                                    ║
 ║             ▼                      ▼                                    ║
 ║  1. 基础领域层 (Core Primitives)                                         ║
 ║     sublift_core: Models, ImageView, ImageBuffer, Strong Boxes, Config ║
 ║                                                                         ║
 ║  3. 抽象契约层 (Ports / Abstractions)                                    ║
 ║     sublift_ports: IFrameSource, IOcrEngine, ISubtitleSink, IDetector    ║
 ║                                                                         ║
 ╚══════════════════════════════════▲══════════════════════════════════════╝
                                    │ 实现抽象契约 (Adapters -> Ports)
 5. 适配器层 (Adapters / Plumbing)
    sublift_adapter_mock (M0), ffmpeg, paddle, apple_vision
    【仅依赖 ports 与 core；不感知 engine、timing、text】
```

### 1.1 层级职责定义

1. **基础领域层 (Layer 1: Core Primitives)**
   - 定义全域通用的纯领域模型、强类型几何空间、像素视图与拥有式像素缓冲区、统一颜色格式契约及错误模型。
   - 依赖边界：纯标准 C++20，零外部三方依赖，无 I/O，无系统调用。
2. **领域算法层 (Layer 2: Domain Pipeline Algorithms)**
   - 划分为三个正交的纯算法模块：
     - **`features`**：视觉特征提取。负责二值化掩码生成、64 位差异哈希（dHash）、掩码结构相似度（SSIM）及核心走廊几何过滤。私有依赖 OpenCV 实现。
     - **`timing`**：时间轴状态推进与字幕段切分。驱动 Empty/Stable 状态转移，执行巡检与代表帧择优收集。输出纯粹的 `Segment`。
     - **`text`**：文本后处理与序列化。执行 OCR 行评分、多候选帧文本共识仲裁、标点清洗规范化、4-Pass 去重合并及 SRT 格式化输出。
3. **抽象契约层 (Layer 3: Ports / Abstractions)**
   - 声明纯虚抽象契约：抽帧数据源（`IFrameSource`）、文字识别引擎（`IOcrEngine`）、字幕结果通道（`ISubtitleSink`）以及字幕区域探测器（`IDetector`）。
   - 依赖反转中枢：彻底解耦抽帧规划与区域检测，隔离具体外部技术实现。
4. **编排层 (Layer 4: Engine Orchestrator)**
   - 统一入口门面（`ExtractionSession`），协调抽帧、特征提取、打轴、OCR 识别与文本共识的端到端生命周期。
   - 内部封装并发与流水线调度策略，通过 `IExtractionObserver` 向宿主分发进度与事件。
5. **适配器层 (Layer 5: Adapters / Plumbing)**
   - Ports 抽象接口的技术实现。负责与外部操作系统、硬件加速框架、文件系统及 AI 推理运行时交互。
   - M0 阶段提供 `adapters/mock`，后续扩展 FFmpeg、PaddleOCR (ONNX Runtime)、Apple Vision 等实现。

---

## 2. 7 个 CMake Target 规范与代码布局

算法核与基础设施由 7 个明确职责的 CMake Target 组成，废除所有根级转发头，统一使用精确命名空间路径包含：

| # | Target 名称 | 模块类型 | 头文件路径 | 源码路径 | 职责范围 |
|---|---|---|---|---|---|
| 1 | `sublift_core` | 静态库 | `cpp/include/sublift/core/` | `cpp/src/core/` | 强类型坐标系统、`ImageView`、`ImageBuffer`、时间戳、领域模型 |
| 2 | `sublift_features` | 静态库 | `cpp/include/sublift/features/` | `cpp/src/features/` | 视觉分析、二值化掩码、dHash、SSIM 巡检、几何走廊 |
| 3 | `sublift_timing` | 静态库 | `cpp/include/sublift/timing/` | `cpp/src/timing/` | 变化点状态机、时间轴累积、代表帧保留 |
| 4 | `sublift_text` | 静态库 | `cpp/include/sublift/text/` | `cpp/src/text/` | 行评分选择、多帧共识、文本清洗、4-Pass 去重、SRT 序列化 |
| 5 | `sublift_ports` | 接口库 (INTERFACE) | `cpp/include/sublift/ports/` | （纯虚接口定义） | `IFrameSource`、`IOcrEngine`、`ISubtitleSink`、`IDetector` |
| 6 | `sublift_engine` | 静态库 | `cpp/include/sublift/engine/` | `cpp/src/engine/` | `ExtractionSession`、`ExtractionConfig`、`IExtractionObserver` |
| 7 | `sublift_adapter_mock` | 静态库 | `cpp/adapters/mock/include/sublift/adapters/mock/` | `cpp/adapters/mock/src/` | 假抽帧源、假 OCR 引擎、内存字幕 Sink |

### 2.1 源码目录树

```text
cpp/
├── CMakeLists.txt
├── cmake/
│   ├── Dependencies.cmake
│   └── DependencyCheck.cmake
├── include/
│   └── sublift/
│       ├── core/
│       │   ├── box.hpp
│       │   ├── color.hpp
│       │   ├── image_view.hpp
│       │   ├── image_buffer.hpp
│       │   ├── frame.hpp
│       │   ├── models.hpp
│       │   ├── stage_timing.hpp
│       │   └── version.hpp
│       ├── features/
│       │   ├── mask.hpp
│       │   ├── dhash.hpp
│       │   ├── ssim.hpp
│       │   ├── geometry.hpp
│       │   └── visual_analysis.hpp
│       ├── timing/
│       │   ├── changepoint.hpp
│       │   ├── timeline.hpp
│       │   ├── segment.hpp
│       │   └── timing_config.hpp
│       ├── text/
│       │   ├── line_select.hpp
│       │   ├── consensus.hpp
│       │   ├── cleaning.hpp
│       │   ├── dedupe.hpp
│       │   └── srt.hpp
│       ├── ports/
│       │   ├── frame_source.hpp
│       │   ├── ocr_engine.hpp
│       │   ├── subtitle_sink.hpp
│       │   └── detector.hpp
│       └── engine/
│           ├── session.hpp
│           ├── config.hpp
│           └── observer.hpp
├── src/
│   ├── core/
│   ├── features/
│   ├── timing/
│   ├── text/
│   └── engine/
├── adapters/
│   └── mock/
│       ├── CMakeLists.txt
│       ├── include/sublift/adapters/mock/
│       │   ├── mock_frame_source.hpp
│       │   ├── mock_ocr.hpp
│       │   └── mock_subtitle_sink.hpp
│       └── src/
└── tests/
    ├── CMakeLists.txt           # 按 unit / parity / adapter / e2e 分层注册 CTest LABELS
    ├── core_test / image_test / models_test
    ├── features_test / adversarial_stress_test
    ├── timing_test / timing_adversarial_stress_test / timeline_adversarial_stress_test
    ├── line_select_test / dedupe_test / text_adversarial_test / region_cluster_*
    ├── mock_adapters_test / engine_test / auto_region_test / adversarial/engine_adversarial_test
    ├── *_parity_test & parity/{paddle,pipeline}_parity_test
    ├── extractor_* / media_info / roi_conversion / paddle_* / ocr_execution / resource_locator
    └── integration/{pipeline_integration_test,pipeline_e2e_smoke_test}.cpp
```

---

## 3. 严格单向依赖与隔离原则

### 3.1 依赖关系矩阵

| Target | 允许链接的 Target (`target_link_libraries`) | 严禁包含或链接的内容 |
|---|---|---|
| `sublift_core` | 无（纯标准库） | 所有其他 Target，任何外部三方库 |
| `sublift_features` | `PUBLIC: sublift_core`<br>`PRIVATE: OpenCV::core OpenCV::imgproc` | `timing`, `text`, `ports`, `engine`, `adapters` |
| `sublift_timing` | `PUBLIC: sublift_core sublift_features` | `text`, `ports`, `engine`, `adapters`, 任何外部三方库 |
| `sublift_text` | `PUBLIC: sublift_core` | `features`, `timing`, `ports`, `engine`, `adapters`, 任何外部三方库 |
| `sublift_ports` | `INTERFACE: sublift_core` | `features`, `timing`, `text`, `engine`, `adapters`, 任何外部三方库 |
| `sublift_engine` | `PUBLIC: sublift_core sublift_ports sublift_timing sublift_text` | `features`（仅通过 timing 间接使用）, `adapters`, 任何外部三方库 |
| `sublift_adapter_mock` | `PUBLIC: sublift_ports sublift_core` | `engine`, `timing`, `features`, `text` |

### 3.2 架构硬约束不变式 (Architectural Invariants)

1. **Timing 与 Text 强互斥规则 (Mutual Exclusion)**
   - `sublift_timing` 与 `sublift_text` 彼此完全不可见。`timing` 严禁 `#include <sublift/text/...>`，`text` 严禁 `#include <sublift/timing/...>`。两者由 `engine` 进行协同编排。
2. **Features 私有包含与单一消费者规则 (Feature Containment)**
   - `sublift_features` 仅由 `sublift_timing` 链接和调用。`core`、`text`、`ports`、`engine`、`adapters` 严禁直接依赖 `features`。
3. **Engine 控制反转规则 (Dependency Inversion)**
   - `sublift_engine` 绝不链接任何 Adapter。所有数据抽帧源、OCR 识别引擎均以接口指针形式由组合根注入。
4. **Adapter 内向单向依赖 (Inward Dependency)**
   - 适配器只能作为契约的实现者（依赖 `ports` 与 `core`），严禁反向依赖 `engine`。
5. **算法核零泄漏规则 (Zero Leakage of 3rd-Party Types)**
   - `core`、`timing`、`text`、`ports`、`engine` 头文件与公共接口中绝不允许出现任何第三方类型（如 `cv::Mat`、`Ort::Value`、`AVFrame` 等）。
   - OpenCV 严格限制为 `sublift_features` 的 `PRIVATE` 依赖；公共头文件完全隔离 OpenCV 符号。
6. **双重依赖防护门禁 (Dual Guard)**
   - 构建期门禁：CMake 显式 target 链接图拓扑隔离；
   - 源码期门禁：独立静态分析脚本扫描所有 include 关系与关键字，违规立即中断构建并报错。

---

## 4. 核心领域数据模型 (Core Domain Models)

### 4.1 强类型坐标系统

为杜绝跨坐标系数值误赋，定义相互不兼容的强类型坐标包围盒：

```cpp
namespace sublift {

struct Box2i {
  std::int32_t x{0};
  std::int32_t y{0};
  std::int32_t width{0};
  std::int32_t height{0};

  [[nodiscard]] constexpr bool empty() const noexcept {
    return width <= 0 || height <= 0;
  }
  friend constexpr bool operator==(const Box2i&, const Box2i&) = default;
};

// 视频全画幅坐标系（源视频全帧尺寸）
struct SourceBox {
  std::int32_t x{0};
  std::int32_t y{0};
  std::int32_t width{0};
  std::int32_t height{0};
  friend constexpr bool operator==(const SourceBox&, const SourceBox&) = default;
};

// 抽帧产物局部坐标系（经过粗剪/ROI 后的图像帧内）
struct FrameLocalBox {
  std::int32_t x{0};
  std::int32_t y{0};
  std::int32_t width{0};
  std::int32_t height{0};
  friend constexpr bool operator==(const FrameLocalBox&, const FrameLocalBox&) = default;
};

// OCR 识别局部坐标系（送入 OCR 引擎的裁剪块坐标）
struct OcrCropBox {
  std::int32_t x{0};
  std::int32_t y{0};
  std::int32_t width{0};
  std::int32_t height{0};
  friend constexpr bool operator==(const OcrCropBox&, const OcrCropBox&) = default;
};

struct Region {
  FrameLocalBox box{};
};

} // namespace sublift
```

### 4.2 像素缓冲与颜色格式契约

颜色契约统一为 `RGB24` 与 `Gray8`，消除历史 BGR 隐式对调怪癖：

```cpp
namespace sublift {

enum class PixelFormat : std::uint8_t {
  RGB24 = 0,
  Gray8 = 1,
};

[[nodiscard]] constexpr int bytes_per_pixel(PixelFormat format) noexcept {
  return (format == PixelFormat::RGB24) ? 3 : 1;
}

// 只读切片视图（借用语义，O(1) 无分配 ROI 切片）
class ImageView {
 public:
  ImageView() = default;
  ImageView(const std::uint8_t* data, std::int32_t width, std::int32_t height,
            std::int32_t stride_bytes, PixelFormat format) noexcept;

  [[nodiscard]] const std::uint8_t* data() const noexcept;
  [[nodiscard]] std::int32_t width() const noexcept;
  [[nodiscard]] std::int32_t height() const noexcept;
  [[nodiscard]] std::int32_t stride_bytes() const noexcept;
  [[nodiscard]] PixelFormat format() const noexcept;
  [[nodiscard]] bool empty() const noexcept;

  [[nodiscard]] ImageView roi(std::int32_t x, std::int32_t y,
                              std::int32_t w, std::int32_t h) const;
  [[nodiscard]] const std::uint8_t* row(std::int32_t y) const;
};

// 拥有式像素缓冲区（独占内存移动语义，支持免零初始化优化）
class ImageBuffer {
 public:
  ImageBuffer() = default;
  ImageBuffer(std::int32_t width, std::int32_t height, PixelFormat format,
              std::int32_t stride_bytes = 0);

  [[nodiscard]] static ImageBuffer allocate_uninitialized(
      std::int32_t width, std::int32_t height, PixelFormat format,
      std::int32_t stride_bytes = 0);

  ImageBuffer(ImageBuffer&&) noexcept;
  ImageBuffer& operator=(ImageBuffer&&) noexcept;
  ImageBuffer(const ImageBuffer&) = delete;
  ImageBuffer& operator=(const ImageBuffer&) = delete;

  [[nodiscard]] ImageView view() const noexcept;
  [[nodiscard]] ImageBuffer clone() const;
  [[nodiscard]] bool empty() const noexcept;
};

struct Frame {
  std::int64_t timestamp_ms{0};
  ImageBuffer image{};
};

} // namespace sublift
```

### 4.3 时间戳与字幕领域模型

全域配置与状态数值严格以毫秒（ms）为统一时间基准：

```cpp
namespace sublift {

using Timestamp = std::int64_t; // 毫秒

struct TimeRange {
  Timestamp start_ms{0};
  Timestamp end_ms{0};

  [[nodiscard]] constexpr Timestamp duration_ms() const noexcept {
    return end_ms - start_ms;
  }
  friend constexpr bool operator==(const TimeRange&, const TimeRange&) = default;
};

struct SubtitleItem {
  std::uint64_t id{0};
  TimeRange range{};
  std::string text{};
  double confidence{0.0};
  friend bool operator==(const SubtitleItem&, const SubtitleItem&) = default;
};

using SubtitleEntry = SubtitleItem;

} // namespace sublift
```

---

## 5. Ports 抽象契约规范

位于 `cpp/include/sublift/ports/`，定义算法核对系统 I/O 与推理硬件的无泄漏抽象接口。

### 5.1 抽帧源契约 (`ports/frame_source.hpp`)

```cpp
namespace sublift {

struct VideoInfo {
  std::int32_t width{0};
  std::int32_t height{0};
  std::int64_t duration_ms{0};
  double fps{0.0};
  friend bool operator==(const VideoInfo&, const VideoInfo&) = default;
};

struct SourceFrameInfo {
  std::int32_t width{0};
  std::int32_t height{0};
  bool display_transform_ok{true};
  std::optional<std::string> transform_note;
  std::int32_t pixel_aspect_num{1};
  std::int32_t pixel_aspect_den{1};
  std::int64_t relative_video_start_ms{0};
  friend bool operator==(const SourceFrameInfo&, const SourceFrameInfo&) = default;
};

enum class FrameOutputMode {
  FullRgb,
  RoiRgb
};

// 纯数据决策计划：FrameIOPlan 不持有 IDetector 指针，消除职责倒置
struct FrameIOPlan {
  std::optional<SourceBox> output_crop;
  FrameOutputMode output_mode{FrameOutputMode::FullRgb};
  std::optional<SourceFrameInfo> source;
  std::optional<std::string> fallback_reason;
  friend bool operator==(const FrameIOPlan&, const FrameIOPlan&) = default;
};

class IFrameSource {
 public:
  virtual ~IFrameSource() = default;

  [[nodiscard]] virtual VideoInfo probe(const std::filesystem::path& video_path) = 0;

  [[nodiscard]] virtual FrameIOPlan plan(
      const VideoInfo& info,
      const std::optional<SourceBox>& requested_box) = 0;

  virtual void extract(
      const std::filesystem::path& video_path,
      double fps,
      const std::optional<SourceBox>& crop,
      std::function<bool(Frame)> consumer) = 0;

  [[nodiscard]] virtual ImageBuffer extract_frame(
      const std::filesystem::path& video_path,
      std::int64_t timestamp_ms,
      const std::optional<SourceBox>& crop) = 0;

  virtual void cancel() = 0;
  [[nodiscard]] virtual bool is_cancelled() const noexcept = 0;
};

} // namespace sublift
```

### 5.2 OCR 引擎契约 (`ports/ocr_engine.hpp`)

```cpp
namespace sublift {

struct OcrLine {
  std::string text;
  double confidence{0.0};
  OcrCropBox box{};
  friend bool operator==(const OcrLine&, const OcrLine&) = default;
};

struct OcrResult {
  std::string text;
  double confidence{0.0};
  std::vector<OcrLine> lines{};

  static OcrResult from_lines(std::span<const OcrLine> lines);
  friend bool operator==(const OcrResult&, const OcrResult&) = default;
};

class IOcrEngine {
 public:
  virtual ~IOcrEngine() = default;

  IOcrEngine(const IOcrEngine&) = delete;
  IOcrEngine& operator=(const IOcrEngine&) = delete;
  IOcrEngine(IOcrEngine&&) = default;
  IOcrEngine& operator=(IOcrEngine&&) = default;

  // 输入只读借用切片，避免深拷贝，内部严禁持久保留内存引用
  [[nodiscard]] virtual OcrResult recognize(const ImageView& image) = 0;

  [[nodiscard]] virtual std::string engine_name() const = 0;
};

} // namespace sublift
```

### 5.3 输出通道契约 (`ports/subtitle_sink.hpp`)

```cpp
namespace sublift {

class ISubtitleSink {
 public:
  virtual ~ISubtitleSink() = default;

  virtual void emit(const SubtitleEntry& entry) = 0;
  virtual void flush() = 0;
  virtual void close() = 0;
};

} // namespace sublift
```

### 5.4 字幕区域探测器契约 (`ports/detector.hpp`)

```cpp
namespace sublift {

class IDetector {
 public:
  virtual ~IDetector() = default;

  [[nodiscard]] virtual std::optional<Region> detect(const Frame& frame) = 0;
};

} // namespace sublift
```

---

## 6. 引擎编排层与公共 API (Engine Public API)

`sublift_engine` 提供面向宿主层的会话入口，实现统一参数收敛与事件观察。

### 6.1 统一执行配置 (`engine/config.hpp`)

配置项全量收敛至毫秒语义，消除旧项目帧数与毫秒双轨冗余：

```cpp
namespace sublift {

struct ExtractionConfig {
  // 采样参数
  double sample_fps{5.0};
  double region_bottom_ratio{0.3};

  // 打轴状态机参数（纯毫秒基准）
  std::int64_t hysteresis_ms{200};
  std::int64_t confirm_ms{200};
  std::int64_t patrol_interval_ms{600};
  std::int32_t change_threshold{10};
  double presence_threshold{0.01};
  double ssim_patrol_threshold{0.92};
  double aspect_ratio_threshold{6.0};

  // 文本筛选与共识参数
  double confidence_threshold{0.5};
  double low_conf_threshold{0.28};
  double line_select_min_score{0.28};
  double line_select_min_script{0.12};
  std::int32_t ocr_consensus_frames{4};
  std::int64_t min_duration_ms{500};
  std::int64_t merge_gap_ms{1000};
  std::string subtitle_script{"auto"};
  bool drop_empty_text{false};

  // 调度并发控制
  std::size_t num_threads{0};    // 0 表示自适应系统物理核心数
  std::size_t queue_capacity{16}; // 流水线背压深度
  bool pure_timing{false};       // 纯打轴模式（不保留图像以节约内存）

  friend bool operator==(const ExtractionConfig&, const ExtractionConfig&) = default;
};

} // namespace sublift
```

### 6.2 观察者与事件通知 (`engine/observer.hpp`)

```cpp
namespace sublift {

enum class ExtractionErrorCode {
  SourceReadError,
  DecoderFailed,
  OcrFailed,
  InvalidConfiguration,
  Aborted
};

struct ExtractionError {
  ExtractionErrorCode code;
  std::string message;
  std::int64_t timestamp_ms{0};
};

struct ExtractionMetrics {
  std::uint64_t total_frames_processed{0};
  std::uint64_t total_segments_detected{0};
  std::uint64_t total_ocr_calls{0};
  std::uint64_t total_entries_produced{0};
  std::int64_t wall_time_ms{0};
  double effective_fps{0.0};
};

class IExtractionObserver {
 public:
  virtual ~IExtractionObserver() = default;

  virtual void on_progress(std::int64_t current_ms, std::int64_t total_ms, double fps) {}
  virtual void on_segment_detected(const timing::Segment& segment) {}
  virtual void on_subtitle_entry(const SubtitleEntry& entry) {}
  virtual void on_error(const ExtractionError& error) {}
  virtual void on_complete(const ExtractionMetrics& metrics) {}
};

} // namespace sublift
```

### 6.3 会话主接口 (`engine/session.hpp`)

```cpp
namespace sublift {

class ExtractionSession {
 public:
  // 依赖倒置构造：注入抽象接口与配置
  explicit ExtractionSession(
      IFrameSource& frame_source,
      IOcrEngine& ocr_engine,
      IDetector* detector = nullptr,
      ISubtitleSink* sink = nullptr,
      ExtractionConfig config = {});

  ~ExtractionSession();

  ExtractionSession(const ExtractionSession&) = delete;
  ExtractionSession& operator=(const ExtractionSession&) = delete;
  ExtractionSession(ExtractionSession&&) = delete;
  ExtractionSession& operator=(ExtractionSession&&) = delete;

  void set_observer(IExtractionObserver* observer);

  // 阻塞执行全片提取流程
  std::vector<SubtitleEntry> process_video(
      const std::filesystem::path& video_path,
      const std::optional<SourceBox>& region_box = std::nullopt);

  // 流式交互入口：逐帧推入与排空结束
  void feed(Frame frame);
  std::vector<SubtitleEntry> finalize();

  // 协作式主动取消
  void cancel();

  [[nodiscard]] bool is_running() const noexcept;
  [[nodiscard]] bool is_cancelled() const noexcept;
  [[nodiscard]] ExtractionMetrics metrics() const noexcept;
  [[nodiscard]] const ExtractionConfig& config() const noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace sublift
```

---

## 7. 全链路数据流与并发执行模型

```text
 1. 外部视频流
    │ [IFrameSource::extract(video_path, fps, consumer)]
    ▼
 2. 解码与几何抽取
    Frame { timestamp_ms, image(RGB24) }
    │
    ▼ [ExtractionSession::feed]
 3. 视觉特征提取 (features)
    ImageView::roi(region.box)
    ├── features::compute_visual_analysis
    │     ├── 二值化掩码 valid_mask (Gray8)
    │     └── 64-bit dHash
    └── FrameVisualAnalysis { FrameSignature, valid_mask }
    │
    ▼
 4. 变化点状态机推进 (timing)
    ChangePointDetector::process
    ├── 状态转移 (EMPTY -> STABLE -> STABLE_PRIME)
    ├── SSIM 巡检 (SsimWorkspace 零分配复用, AnchorMaskCache)
    └── 产生 StateEvent { IN, OUT, CHANGE }
    │
    ▼
 5. 段事件与代表帧保留 (timing)
    TimelineBuilder::consume
    └── 生成 Segment { start_ms, end_ms, anchor_frame, fallback_frames }
    │
    ▼
 6. 文本识别 (ports::IOcrEngine)
    IOcrEngine::recognize(rep_frame.image.view())
    └── OcrResult { text, confidence, lines: vector<OcrLine> }
    │
    ▼
 7. 文本筛选与共识仲裁 (text)
    ├── text::score_line & select_line (基于 SubtitleProfile 几何/语种打分)
    ├── text::consensus_text (多代表帧编辑距离聚类与投票共识)
    └── text::cleanup_subtitle_text (标点规范化与空白字符清洗)
    └── SubtitleEntry { start_ms, end_ms, text, confidence }
    │
    ▼
 8. 后期去重与序列化 (text + ports)
    ├── text::merge_entries (4-Pass: 相邻碎片合并, 极短过滤, 空白剔除)
    ├── text::format_srt (标准时间轴 hh:mm:ss,ms 与 Cue 序号生成)
    └── ISubtitleSink::emit / 文件输出
```

### 7.1 并发与调度拓扑

- **背压帧队列 (Backpressure Queue)**：抽帧线程与分析线程之间设置有界队列（容量由 `ExtractionConfig::queue_capacity` 控制），防止解码超前导致内存无限制增长。
- **并行特征分析 (Parallel Visual Analysis)**：使用工作线程池无序并发计算帧掩码与 64 位 dHash。
- **单线程有序收敛 (Ordered Convergence)**：按时间戳升序有序回放至单线程 `ChangePointDetector` 状态机，保证打轴决策 100% 确定性。
- **异步 OCR 任务队列 (Async OCR Pipeline)**：段闭合后产生的代表帧提交至 OCR 线程池执行，OCR 结果流向 `text` 模块进行共识决策，最终由主线程执行 4-Pass 去重。

---

## 8. 第三方库封装与隔离机制

1. **OpenCV 封装策略**：
   - 仅允许在 `sublift_features` 内部链接：
     ```cmake
     target_link_libraries(sublift_features
       PUBLIC sublift_core
       PRIVATE OpenCV::core OpenCV::imgproc
     )
     ```
   - 公共头文件 (`include/sublift/features/*.hpp`) 绝对严禁 `#include <opencv2/...>`。
   - 掩码缓存与 SSIM 工作区采用私有实现类（Pimpl）隔离 `cv::Mat` 内部指针。
2. **AI 推理引擎 (ONNX Runtime / Apple Vision) 隔离策略**：
   - 纯算法层（`core`、`features`、`timing`、`text`、`ports`、`engine`）完全不包含任何推理引擎头文件或依赖。
   - 所有模型输入与输出全部下沉至 `adapters/paddle` 与 `adapters/apple_vision`。
3. **多媒体解码 (FFmpeg) 隔离策略**：
   - 算法核不链接 `libavcodec` / `libavformat`，通过 `ports::IFrameSource` 隔离具体媒体解码技术。

---

## 9. 质量保障与分层测试设计

CTest 通过 `LABELS` 将全部测试划分为四层，兼顾日常迭代速度与完整契约验收：

- `unit`：纯内存与 Mock 单元测试（`core_test`、`features_test`、`timing_test`、`line_select_test`、`dedupe_test`、`region_cluster_test`、`mock_adapters_test`、`engine_test`、`pipeline_integration_test`、`ffmpeg_unit_test`、`paddle_unit_test` 及 Python 依赖规则测试 `test_dependency_rules` / `test_dependency_negative_cases`），日常开发通过 `ctest -L unit` 快速验证。
- `parity`：与参考基线（golden JSON）逐项对齐的契约一致性测试（`signature_parity_test`、`changepoint_parity_test`、`timeline_parity_test`、`line_select_parity_test`、`dedupe_parity_test`、`pipeline_parity_test`），含适配器基线的 `extractor_parity_test` 与 `paddle_parity_test` 同时带有 `parity;adapter` 双标签。
- `adapter`：依赖外部 FFmpeg 或 ONNX Runtime 的适配器集成测试（`ffmpeg_adapter_test`、`paddle_adapter_test`）。
- `e2e`：基于真实短片段的端到端管线冒烟测试（`pipeline_e2e_smoke_test`，门禁阈值统一读取 `benchmark/gates.json` 并调用 `scripts/srt_eval.py` 评估）。

| 领域 / 层次 | 测试目标 | 验证核心逻辑 |
|---|---|---|
| `sublift_core` / `ports` | `core_test` | `ImageView`、`ImageBuffer`、坐标空间、版本与契约多态调用及边界压力测试 |
| `sublift_features` | `features_test` | 视觉分析接口、掩码生成、64 位 dHash、SSIM 与走廊裁剪 |
| `sublift_timing` | `timing_test` | 变化点状态机、时间轴累积、等价性与异常时间轴抗压测试 |
| `sublift_text` | `line_select_test`、`dedupe_test`、`region_cluster_test` | OCR 行选与共识、4-Pass 去重合并、SRT 格式化、自动区域聚类 |
| `sublift_engine` / `mock` | `mock_adapters_test`、`engine_test`、`pipeline_integration_test` | `ExtractionSession`、`AnalysisExecutor`、`SegmentRecognizer`、取消背压与多帧流端到端 Mock 编排 |
| `sublift_adapter_ffmpeg` | `ffmpeg_unit_test`、`ffmpeg_adapter_test`、`extractor_parity_test` | ROI 变换、旋转矩阵、探针、两阶段裁剪与真实 FFmpeg 抽帧 |
| `sublift_adapter_paddle` | `paddle_unit_test`、`paddle_adapter_test`、`paddle_parity_test` | DB 后处理、CTC 解码、模型定位与缓存探针、真实 ORT/Paddle 推理对齐 |
| 全链路短片 E2E | `pipeline_e2e_smoke_test` | 30.5s 真实视频片段提取，通过 `scripts/srt_eval.py` 校验 `benchmark/gates.json` 门禁 |

### 9.1 分层触发时机

| 开发 / 验收阶段 | 推荐命令 | 覆盖范围与适用时机 |
|---|---|---|
| 日常开发内循环 | `ctest --test-dir build/debug -L unit --output-on-failure` | 每次修改代码后执行，覆盖纯内存/Mock 单测与架构/测试卫生静态检查 |
| 算法/契约回归 | `ctest --test-dir build/debug -L parity --output-on-failure` | 修改 `features`/`timing`/`text`/`engine` 算法行为或提取/OCR 输出契约时执行 |
| 适配器与构建变更 | `ctest --test-dir build/debug -L adapter --output-on-failure` | 修改 `cpp/adapters/`、`cpp/cmake/` 或 `tools/` 时执行，验证真实 FFmpeg / ONNX Runtime 短样本适配 |
| 合入 `dev` 前全量验收 | `ctest --test-dir build/debug --output-on-failure` | 工作分支收口提交或合并至 `dev` 前必跑，覆盖全量 `unit` + `parity` + `adapter` + 30.5s 短片 `e2e` |
| 性能/并发/发布门禁 | `./scripts/run_e2e_tests.sh --all` 或 `./scripts/run_e2e_benchmark.sh`（基于 `build/release`） | 仅在涉及性能、并发、门禁阈值变更或里程碑收口 / 合入 `main` 前执行；包含 CLI 边界、SIGINT、40s 并发确定性与 254s 整片 3 路性能/质量/RSS 门禁，不进入日常 `ctest` |

### 9.2 测试准入红线

1. **禁止随意新建测试二进制**：新用例必须并入所属模块的既有 `*_test` 目标，严禁新增 `smoke_*`、`*_adversarial*`、`*_stub*` 等独立可执行文件；所有 `add_test` 必须声明合法 `LABELS`（`unit` / `parity` / `adapter` / `e2e`）。
2. **禁止重型负载跨层混入**：`unit` 标签严禁启动真实 `ffmpeg`/`ffprobe`、加载真实 ORT/Paddle 模型或读写大体积媒体；完整长视频严禁进入 `ctest`（只归 `scripts/run_e2e_benchmark.sh`）。
3. **禁止名义测试与自证代码**：严禁在测试中重写算法自证、严禁直接 `popen` 测外部命令行工具本身、严禁只验证“构造不崩”或 `time >= 0` 的无效用例。
4. **禁止硬编码墙钟耗时断言与 `sleep` 竞态**：功能单测严禁出现 `elapsed_ms < N` 类脆弱墙钟断言（性能指标统一归 `benchmark/`）；并发、取消与背压测试必须使用 `std::promise` / 条件变量等确定性同步原语，严禁靠 `sleep_for` 猜时序或撞超时。
5. **禁止散落门禁阈值与重复评估器**：端到端阈值唯一权威源为 [benchmark/gates.json](../benchmark/gates.json)，SRT 评估唯一实现为 [scripts/srt_eval.py](../scripts/srt_eval.py)。
