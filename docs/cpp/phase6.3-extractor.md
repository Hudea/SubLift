# Phase 6.3 — FFmpeg Extractor 行为复刻

> 子阶段编码：`S = 3` → feature 前缀 `feat-063xx`  
> 任务跟踪：`docs/phases/phase6.json`  
> 总览：[phase6-overview.md](phase6-overview.md)  
> 契约：[architecture.md](architecture.md) · [parity-contract.md](parity-contract.md) · ROI 语义 [../design/roi-data-path.md](../design/roi-data-path.md)  
> **门槛：** `feat-06201`–`feat-06205` 全部 `done`（已满足）  
> **Oracle：** `src/sublift/extractor/`（`ffmpeg_extractor.py` · `frame_io.py` · `base.py`）

## 1. 目标

在 **不切换产品默认 runtime** 的前提下，把 Python **ffmpeg subprocess 抽帧**迁入 `sublift_ffmpeg`（接口可在 `sublift_core` 声明），与冻结 Oracle **行为 parity**：

```text
resolve ffmpeg/ffprobe
    → probe source-frame (w/h + display_transform_ok)
    → [optional] validate_output_crop (strict, no clamp)
    → build_output_vf(fps, crop?)
    → spawn ffmpeg -nostdin … -vf … -f image2pipe -pix_fmt rgb24 -vcodec rawvideo -
    → read fixed-size RGB frames
    → timestamp_ms = int(frame_index / fps * 1000)
    → yield Frame{ timestamp_ms, ImageBuffer RGB24 }
    → cancel → terminate/kill child
```

| 交付 | 说明 |
|---|---|
| `IExtractor` + `FfmpegExtractor` | 与 Python Protocol / 实现语义对齐 |
| Probe / crop / vf 纯函数 | 可单测、可 golden，不依赖真实视频也可 lock 字符串与几何 |
| `plan_frame_io` + ROI 装配 | auto/full/roi + display_transform 回退；产出 `FrameIOPlan` |
| Detector 补齐 | `BottomCropDetector`、`RoiPassthroughDetector`（ROI 语义边界） |
| Golden harness | 合成短视频 / 固定 fixture → 帧数、ts、尺寸、ROI 像素抽样 L0 |

**本子阶段结束时：产品 CLI/GUI 仍走 Python。** 6.2 `Pipeline` 可继续用合成帧；6.3 交付的是 **可替换的真实抽帧后端**，供后续 worker / 集成，但不做 cutover。

## 2. 做 / 不做

### 做

1. **系统 ffmpeg/ffprobe subprocess**（PATH + Homebrew 候选路径，与 Python 列表对齐）。
2. **全帧**与 **source-frame ROI crop-before-C++** 两路径（`output_crop`；vf 链与 Python `build_output_vf` **字符串 L0**）。
3. **严格** `validate_output_crop`（禁止 clamp；错误信息含 source / requested）。
4. **display transform 保守策略**：rotate / non-identity display matrix → `display_transform_ok=false`；`plan_frame_io` auto 回退全帧。
5. **索引时间戳** `timestamp_ms = int(frame_index / fps * 1000)`（与 Python 一致，**不是** packet PTS）。
6. **cancel**：协作式 flag + terminate/kill 子进程；已 cancel 的 `extract` 空迭代。
7. **stderr 诊断**：失败时附带尾部（临时文件方案，避免 PIPE 死锁；stdin=DEVNULL / `-nostdin`）。
8. **帧所有权**：每帧 `ImageBuffer` 独有 RGB24 像素；`PixelFormat::RGB24`。
9. Parity dump + Catch2 offline 比对；init.sh `--check`。

### 不做

| 项 | 归属 |
|---|---|
| libav / 进程内 decode | **明确禁止**（全 Phase 6 默认） |
| Vision / Paddle OCR | **6.4** / 引擎矩阵 |
| UDS Worker 切换默认抽帧 | **6.5–6.6** |
| 产品路径 cutover | **6.6** |
| 随 app 打包 ffmpeg 二进制 | **6.7+** |
| PerformanceRecorder 全量复刻 | 6.3 **可选**；parity **不依赖** perf 字段 |
| 顺手「修正」时间戳为 PTS / 修正 yuv crop 色度 | 禁止；以 Oracle 为准 |
| Swift frame-mode JPEG 路径 | 保持 Python/Swift 现状；不在 6.3 |

## 3. 任务一览

| ID | 名称 | 验收一句话 |
|---|---|---|
| **feat-06301** | 纯契约：几何 / vf / display matrix + `IExtractor` | 无 spawn 单测锁 `validate_output_crop`、`build_output_vf`、矩阵判定、接口可编译 |
| **feat-06302** | ffprobe：resolve + probe | 真实/fixture 探测 w/h、duration_ms、transform_ok 与 Python 一致 |
| **feat-06303** | `FfmpegExtractor` 全帧 extract + cancel | 合成视频帧数、ts 序列、尺寸 L0；cancel 不挂死 |
| **feat-06304** | ROI crop + `plan_frame_io` + BottomCrop / RoiPassthrough | ROI 输出尺寸=crop；plan auto/full/roi 与回退语义对齐 |
| **feat-06305** | Extractor golden harness | `dump_extractor.py` + `[parity][extractor]`；init.sh `--check` |

依赖：

```text
feat-06205 (Pipeline e2e done)
    └── feat-06301 pure contracts + IExtractor
            └── feat-06302 ffprobe
                    └── feat-06303 full-frame extract + cancel
                            └── feat-06304 ROI + plan_frame_io + detectors
                                    └── feat-06305 e2e golden
```

**推荐一次一刀：** 06301 → 06302 → 06303 → 06304 → 06305。

---

## 4. 任务详述

### feat-06301 — 纯契约（无 spawn）

**Python oracle：** `validate_output_crop`、`build_output_vf`、`_assess_display_transform` / `_is_identity_display_matrix`、`Extractor` Protocol。

| 必须 | 说明 |
|---|---|
| 类型 | `SourceFrameInfo`、`VideoInfo`（或等价）；`FrameIOPlan` 可先声明后在 06304 填满 |
| `validate_output_crop(SourceBox, w, h)` | 负坐标 / 非正尺寸 / 越界 → 错误；贴边合法；奇数尺寸允许 |
| `build_output_vf(fps, crop?)` | 无 crop：`fps={fps}`；有 crop：`fps={fps},format=rgb24,crop={w}:{h}:{x}:{y}:exact=1` **字符级一致** |
| Display matrix | 与 Python 相同保守规则（tags.rotate、side_data display matrix、16.16 定点恒等、rotation≈0） |
| `IExtractor` | `virtual` 流式 API（见 §5）；header 在 core 或 ffmpeg 公开头 |
| Target | 纯逻辑优先进 **`sublift_ffmpeg`**（或 core 的 `extractor_plan`）；**不**链 ObjC |
| 单测 | 表驱动；**零** ffmpeg 进程 |

**不做：** Popen、读 pipe、真实文件 probe。

---

### feat-06302 — ffprobe 探测

**Python oracle：** `_resolve_bin`、`_probe_source_frame`、`probe_duration_ms`、`probe_video`。

| 必须 | 说明 |
|---|---|
| Bin 候选 | 与 Python 同序：`ffmpeg` / Homebrew ffmpeg-full / `/opt/homebrew/bin` / `/usr/local/bin`（ffprobe 同理） |
| `probe_source_frame(path)` | width/height + `display_transform_ok` + optional note |
| `probe_duration_ms` | format.duration → ms；失败 **0**（与 Python 一致） |
| 错误 | 非 0 退出附 stderr 尾；超时；无视频流 |
| 单测 | integration：短合成视频；pure：可对 ffprobe JSON fixture 喂 `_assess`（若实现拆分） |

**不做：** 抽帧 pipe。

---

### feat-06303 — 全帧 `FfmpegExtractor` + cancel

**Python oracle：** `FfmpegExtractor.extract`（`output_crop=None`）、`cancel`。

| 必须 | 说明 |
|---|---|
| Cmd 形 | `-nostdin -v error -i <path> -vf <vf> -f image2pipe -pix_fmt rgb24 -vcodec rawvideo -` |
| stdin | `DEVNULL`（防 GUI/无 TTY 阻塞） |
| stderr | 临时文件而非 PIPE（防死锁）；失败读尾 |
| 读帧 | `frame_size = w*h*3`；不足一帧 → 结束 |
| ts | `timestamp_ms = int(frame_index / fps * 1000)`（整数除向零，与 Py3 正数一致） |
| 像素 | `ImageBuffer` RGB24，`width/height` = source |
| cancel | 任意阶段可停；杀子进程；不要求抛错 |
| 错误 | 文件不存在 → 明确错误；ffmpeg 非 0 → Runtime 类错误 + stderr |
| 单测 | lavfi/`ffmpeg -f lavfi -i testsrc=…` 生成 320×240 短片；帧数与 ts 列表 L0 |

**不做：** ROI crop；`plan_frame_io`。

---

### feat-06304 — ROI + `plan_frame_io` + Detector 补齐

**Python oracle：** `FfmpegExtractor(output_crop=…)`、`plan_frame_io`、`RoiPassthroughDetector`、`BottomCropDetector`。

| 必须 | 说明 |
|---|---|
| ROI extract | crop 后 `out_w/h = crop`；`Frame.image` 为 **frame-local** ROI |
| spawn 前 | 再次 `validate_output_crop`；非法 **立即失败**，禁止静默全帧 |
| `plan_frame_io` | mode `auto\|full\|roi`；无 region → BottomCrop + full；full → FixedRegion + full；auto/roi + transform 失败策略与 Python 一致 |
| `BottomCropDetector` | 按 `region_bottom_ratio` 下部裁剪（与 Python 几何一致） |
| `RoiPassthroughDetector` | 固定返回 `[0,0,w,h]` frame-local；**永不**吃 source-frame box |
| 单测 | ROI 尺寸；非法 crop 失败；plan fallback_reason；passthrough 不二次裁剪 |

**坐标提醒（P0）：** ROI 路径下 Pipeline 必须用 passthrough detector；把 source box 塞进 FixedRegion 会二次 crop——设计与测试必须锁死。

**不做：** golden 全套（归 06305）；perf 字段。

---

### feat-06305 — Extractor golden harness

| 必须 | 说明 |
|---|---|
| Dump | `scripts/parity/dump_extractor.py`：Python oracle → envelope（schema 版本、oracle 元数据） |
| Fixture | 仓库内小 mp4 **或** dump 时 lavfi 生成并记录生成命令 + 文件 sha256 |
| Golden | 每 scenario：`fps`、`output_crop?`、`frame_count`、`timestamps_ms[]`、`width/height`、可选 **像素抽样**（如每帧 4 角 + 中心 RGB） |
| C++ | `load_extractor_golden` + `[parity][extractor]` 离线跑 Candidate 比对 L0 |
| init.sh | `dump_extractor.py --check` |
| 覆盖 | 至少：全帧 1fps；全帧 2fps ts；ROI crop；非法 crop 期望错误码/类；cancel 中途（可行为断言） |

**比较层：**

| 字段 | 层 |
|---|---|
| frame_count、timestamps_ms、output w/h | L0 |
| vf 字符串（若 dump） | L0 |
| 像素抽样 RGB | L0（合成源） |
| wall time / RSS | **不在 6.3 门禁** |

---

## 5. 建议 C++ API（方向）

```text
// include/sublift/extractor.hpp  (core 或 ffmpeg 对外头)
namespace sublift {

struct SourceFrameInfo {
  int32_t width{0};
  int32_t height{0};
  bool display_transform_ok{true};
  std::optional<std::string> transform_note;
};

struct VideoInfo {
  int32_t width{0};
  int32_t height{0};
  int64_t duration_ms{0};
};

struct FrameIOPlan {
  std::optional<SourceBox> output_crop;   // source-frame；nullopt = full
  // detector 以 unique_ptr/shared 或调用方装配；避免 plan 拥有复杂生命周期歧义
  // 实现可选：plan 只返回 crop + mode + flags，detector 由工厂另建
  enum class OutputMode { FullRgb, RoiRgb } output_mode;
  std::optional<SourceFrameInfo> source;
  std::optional<std::string> fallback_reason;
};

struct IExtractor {
  virtual ~IExtractor() = default;
  // 拉模型：一次 extract 填充 frames，或 callback / 输入迭代器。
  // **推荐** 与 Python 对齐的拉式：
  //   for (Frame& f : extractor.extract(path)) { ... }
  // C++ 可用：
  //   void extract(const std::filesystem::path&, const std::function<bool(Frame)>&);
  //   返回 false 停止；或专用 ExtractSession 带 next().
  virtual void cancel() = 0;
};

}  // namespace sublift

namespace sublift::ffmpeg {

std::string resolve_ffmpeg_bin();   // throws / expected
std::string resolve_ffprobe_bin();

SourceFrameInfo probe_source_frame(const std::filesystem::path&);
int64_t probe_duration_ms(const std::filesystem::path&);
void validate_output_crop(const SourceBox& crop, int32_t sw, int32_t sh);
std::string build_output_vf(double fps, const SourceBox* crop /*nullable*/);

class FfmpegExtractor final : public IExtractor {
public:
  explicit FfmpegExtractor(double fps = 1.0,
                           std::optional<SourceBox> output_crop = std::nullopt,
                           std::optional<SourceFrameInfo> source_info = std::nullopt);
  // extract: yields Frame RGB24; see implementation notes
  void cancel() override;
};

FrameIOPlan plan_frame_io(
    const std::filesystem::path& video_path,
    const SourceBox* region_box,  // nullable
    std::string_view mode /* auto|full|roi */,
    ...);

}  // namespace sublift::ffmpeg
```

### 实现要点（P0）

| 主题 | 契约 |
|---|---|
| Target | 实现进 **`sublift_ffmpeg`**；`sublift_core` 可仅放 `IExtractor` / 纯几何（若想 core 单测不链 ffmpeg） |
| 链接 | ffmpeg **可执行文件**，不 link libav* |
| 线程 | 单 extractor 实例 **非**线程安全；cancel 可从另一线程设 flag（与 Python `Event` 同类） |
| 异常 | 与现有 core 错误风格一致；跨 worker 边界前映射为消息（6.5） |
| 色域 | 输出 **仅 RGB24**；BGR 怪癖仍只在 Pipeline.feed（6.2） |

### 与 6.2 Pipeline 的衔接

```text
FfmpegExtractor::extract
    → Frame(RGB24)
        → Pipeline::feed   // 内部 RGB→BGR quirk → signature
```

6.3 **不要求** 改 Pipeline API；集成冒烟可另加可选 test（非 06305 必达）。

---

## 6. 工程落点

```text
cpp/include/sublift/
  extractor.hpp          # IExtractor + plan 类型（或 ffmpeg.hpp 扩展）
  ffmpeg.hpp             # 替换 stub：probe / FfmpegExtractor
  bottom_crop_detector.hpp
  roi_passthrough_detector.hpp
cpp/src/ffmpeg/
  CMakeLists.txt
  resolve_bin.cpp
  probe.cpp
  validate_crop.cpp
  output_vf.cpp
  display_transform.cpp
  ffmpeg_extractor.cpp
  plan_frame_io.cpp
cpp/src/core/            # detectors 若放 core
  bottom_crop_detector.cpp
  roi_passthrough_detector.cpp
cpp/tests/
  extractor_pure_test.cpp
  extractor_probe_test.cpp      # may require ffprobe
  extractor_full_test.cpp       # requires ffmpeg
  extractor_roi_test.cpp
  parity/extractor_parity_test.cpp
scripts/parity/
  dump_extractor.py
benchmark/parity/
  fixtures/extractor/           # 可选 checked-in 小视频
  goldens/extractor/extractor.v1.json
```

- 删除或收缩 `ffmpeg_stub.cpp` 的 `available()` 为真实探测。  
- CMake：integration 测试可用 `SUBLIFT_HAS_FFMPEG` 探测后 `LABELS integration` 或条件 `if(FFMPEG_EXECUTABLE)`。  
- CI/无 ffmpeg 环境：纯测 + golden offline **仍应绿**（golden 比对不强制本机再 encode，除非标记）。

---

## 7. 风险与已知语义钉扎

| 风险 | 处理 |
|---|---|
| 本机 ffmpeg 版本差导致像素微差 | 合成 testsrc + 抽样容差 **默认 0**（L0）；若实证非确定性再开 epsilon 并记 DECISIONS |
| display matrix 字段随 ffprobe 版本变 | 与 Python 同一保守解析；加 JSON fixture 单测 |
| `int(frame_index/fps*1000)` 浮点 | 与 Python 同一表达式；golden 锁序列 |
| GUI PATH 无 brew | 候选绝对路径列表 **parity** |
| 大视频内存 | 流式读；不一次 load 全片（同 Python） |

---

## 8. 6.3 完成定义

- [x] `feat-06301`–`feat-06305` 全部 `done` + evidence 写入 `phase6.json`
- [x] 全帧 + ROI 抽帧与冻结 golden L0 一致（ts、尺寸、帧数、抽样像素；ROI 含角点/中心）
- [x] `plan_frame_io` auto/full/roi + transform 回退与 Python 一致（纯测 + golden；`plan_frame_io_pure` 注入 source）
- [x] cancel 可终止子进程；失败含 stderr 诊断（pre-cancel 进 golden；mid-flight 仅 unit）
- [x] `ctest` 全绿（integration 运行时 skip 策略见 `cpp/README.md`）；`dump_extractor.py --check`
- [x] **不上 libav**；产品默认仍 Python
- [x] `feature-list` 6.3 块 covers 填齐并标 `done`

> 复核补丁（2026-07-28）：transform fallback / detector type / golden 场景补齐后勾选。

---

## 9. 与后续子阶段

| 之后 | 关系 |
|---|---|
| **6.4 Vision** | 仍吃 `ImageView`；与 extractor 解耦 |
| **6.5 Worker** | C++ worker 内组装 `FfmpegExtractor` + `plan_frame_io` + Pipeline |
| **6.6 Cutover** | 默认 runtime 切换时才用 C++ extract 进产品路径 |
