# Phase 6.2 — Pipeline 流式编排 parity

> 子阶段编码：`S = 2` → feature 前缀 `feat-062xx`  
> 任务跟踪：`docs/phases/phase6.json`  
> 总览：[phase6-overview.md](phase6-overview.md)  
> 契约：[parity-contract.md](parity-contract.md) · [architecture.md](architecture.md)  
> **门槛：** `feat-06101`–`feat-06105` 全部 `done`（已满足）  
> **Oracle：** `src/sublift/pipeline/core.py`（流式 API + 段 OCR 编排）

## 1. 目标

在 **不切换产品默认 runtime** 的前提下，把 Python `Pipeline` 的**流式编排**迁入 `sublift_core`：

```text
feed(frame) → SegmentEvent?
ocr_segment(event) → SubtitleEntry   // raw，进内部列表
finalize() → list[SubtitleEntry]     // 关末段 + merge_entries
cancel()
```

内部串联 **6.1 已 parity** 的积木：

```text
crop → (RGB→BGR 怪癖) → signature → changepoint → timeline
                                      ↓ 段闭合
                         代表帧选取 → OCR(抽象) → line_select/consensus
                                      ↓
                                   closed_entries → finalize/dedupe
```

| 交付 | 说明 |
|---|---|
| C++ `Pipeline` | 与 Python 流式语义对齐的状态机 |
| `IDetector` / `IOcrEngine` | 可替换接口；**公共 API 无 `cv::Mat` / PIL** |
| `MockOcrEngine` + 测试用 Detector | 6.2 parity **不依赖 Vision/ffmpeg** |
| Golden | 段边界、代表帧 ts、OCR 决策、raw/final entries（Mock 下 L0） |

**本子阶段结束时：产品 CLI/GUI 仍走 Python。**

## 2. 做 / 不做

### 做

1. 抽象 `IDetector::detect(Frame) → optional<Region>`、`IOcrEngine::recognize(ImageView) → OcrResult`。
2. 实现 `SegmentEvent`、`Pipeline` 流式四 API + 内部段状态（锚延迟、采样帧、profile）。
3. **复现** feed 路径色域怪癖：`RGB` crop → **BGR** `numpy` 再 `compute_signature`（与 Python `cv2.COLOR_RGB2BGR` 一致）。
4. 代表帧策略与 Python 对齐：延迟锚 `ocr_anchor_delay_frames`、stable/first/fallback 顺序。
5. `ocr_segment`：`enable_line_select` 真/假两路径；Mock OCR 下 text/confidence L0。
6. `finalize`：`timeline.finalize_open_segment` + 末段 OCR + `merge_entries`。
7. `cancel`：标志位 + 清空状态；后续 `feed` 空操作。
8. Parity harness：合成 `Frame` 序列 + Mock OCR → frozen golden；Catch2 offline 比对。

### 不做

| 项 | 归属 |
|---|---|
| ffmpeg subprocess / 真实抽帧 | **6.3** |
| Vision / Paddle 原生 OCR | **6.4** / 引擎矩阵 |
| UDS Worker / Swift 切 C++ | **6.5–6.6** |
| `run(video_path)` 文件批处理一等公民 | 可薄封装，**非 6.2 验收门**（可用 feed 循环等价） |
| TraceRecorder / PerformanceRecorder 全量复刻 | 6.2 **可选**；parity **不依赖** perf 字段 |
| 产品路径 cutover | **6.6** |
| 顺手改打轴/行选阈值/色域 | 禁止 |

## 3. 任务一览

| ID | 名称 | 验收一句话 |
|---|---|---|
| **feat-06201** | 接口 + Mock OCR + 固定 Region Detector | `IDetector`/`IOcrEngine` 可注入；Mock 序列/固定 lines 单测绿 |
| **feat-06202** | `Pipeline::feed` 打轴路径 | 合成帧序列 → SegmentEvent 起止 ms + 代表帧 ts L0 与 Python 一致 |
| **feat-06203** | `Pipeline::ocr_segment` | line_select 开/关 + Mock lines → raw entry text/conf L0 |
| **feat-06204** | `finalize` / `cancel` | 末段关闭 + dedupe 结果 L0；cancel 后 feed 拒绝 |
| **feat-06205** | Pipeline 端到端 golden harness | `dump_pipeline.py` + Catch2 `[parity][pipeline]`；init.sh `--check` |

依赖：

```text
feat-06105 (line_select) + 06104 (dedupe) + 06102/06103
    └── feat-06201 interfaces + mocks
            └── feat-06202 feed
                    └── feat-06203 ocr_segment
                            └── feat-06204 finalize/cancel
                                    └── feat-06205 e2e golden (可与 06204 合并验收，但独立登记)
```

**推荐一次一刀：** 06201 → 06202 → 06203 → 06204 → 06205。

---

## 4. 任务详述

### feat-06201 — 接口 + Mock OCR + 固定 Detector

**Python oracle：** `OcrEngine` Protocol、`MockOcrEngine`；Detector 测试中常用固定 Region。

| 必须 | 说明 |
|---|---|
| `IOcrEngine` | `virtual OcrResult recognize(const ImageView&) = 0`；非拥有 view |
| `MockOcrEngine` | 固定 `OcrResult` **或** 按调用序号的 sequence；越界可 assert/throw |
| `IDetector` | `virtual optional<Region> detect(const Frame&) = 0` |
| `FixedRegionDetector` | 始终返回构造时 Region（测试用） |
| 头文件 | `cpp/include/sublift/ocr.hpp`、`detector.hpp`（或 `pipeline_interfaces.hpp`） |
| 单测 | Mock 序列顺序；detect 返回固定 box |

**不做：** Vision、Paddle、真实视频。

---

### feat-06202 — `Pipeline::feed` 打轴路径

**Python oracle：** `Pipeline.feed`（约 235–303 行）及 `_open_segment` / `_on_stable_frame` / `_close_segment` / `_record_sample_frame`。

| 必须 | 说明 |
|---|---|
| 构造 | `Pipeline(IDetector&, IOcrEngine&, Config)`；extractor **可选且 6.2 可忽略** |
| 首帧 | `detect` → Region；失败则 feed 返回 nullopt |
| Crop | 按 Region 从 `Frame.image` 取 ROI view（不强制拷贝全帧） |
| 色域 | ROI 像素 **按 BGR 布局喂 signature**（与 Python RGB→BGR 一致）；可用 `PixelFormat::BGR24` 标签 + 既有 quirk 路径，**或** 显式转换后仍走 `COLOR_RGB2GRAY` 怪癖——须与 dump 一致并文档化 |
| 状态 | changepoint + timeline + open_segment_start + anchor_frames + sample_frames + delay 计数 |
| 返回 | IN：nullopt（只开段）；OUT/CHANGE：`SegmentEvent{start,end,anchor,fallbacks}` |
| cancel 后 | feed 立即 nullopt |
| Golden 中间量 | 每帧可选 events；每段 `start_ms/end_ms`、`anchor_ts`、`fallback_ts[]` |

**单测：** 无 OCR 调用次数变化下的纯时序（可用 counting mock 断言 0 OCR）。

**不做：** ocr_segment 文本；finalize。

---

### feat-06203 — `Pipeline::ocr_segment`

**Python oracle：** `ocr_segment`、`_ocr_segment_with_line_select`、`_ocr_segment_legacy`、`_ocr_frame_*`。

| 必须 | 说明 |
|---|---|
| 输入 | `SegmentEvent`（含 anchor + fallbacks 的帧图像） |
| line_select 开 | 多代表帧 → select_line → consensus → cleanup → should_accept；不接受则空文本/低 conf 策略与 Python 一致 |
| line_select 关 | legacy join + confidence_threshold + fallback 重试 |
| 输出 | raw `SubtitleEntry` append 到 `_closed_entries` |
| Mock | sequence 模式按 OCR **调用次数** 推进（与 dump 一致） |
| 线程 | **文档声明非线程安全**（同 Python）；单线程串行 |

**不做：** Vision 坐标变换（已在引擎侧；Mock 直接给 lines）。

---

### feat-06204 — `finalize` / `cancel`

| 必须 | 说明 |
|---|---|
| finalize | 若有 open 段：`finalize_open_segment(last_ts)` → close → `ocr_segment`；然后 `merge_entries` 返回新列表 |
| cancel | 设标志；reset changepoint/timeline；清空 frames/entries；profile 回到 config 默认 |
| 单测 | open 段在 finalize 出现；cancel 后 feed 无副作用 |

---

### feat-06205 — 端到端 Pipeline golden harness

| 必须 | 说明 |
|---|---|
| Dump | `scripts/parity/dump_pipeline.py`：Python Pipeline + Mock + 合成帧 → envelope |
| Fixture | `benchmark/parity/fixtures/pipeline/` 小 RGB 帧序列或程序化生成 |
| Golden | `benchmark/parity/goldens/pipeline/pipeline.v1.json`：scenarios + segment events + ocr decisions + raw entries + final entries |
| C++ | `load_pipeline_golden` + `[parity][pipeline]` 离线比对 |
| init.sh | `dump_pipeline.py --check` |
| 覆盖 | 至少：IN/OUT 单段；CHANGE 两段；line_select on；cancel 不污染；finalize 开段 |

**比较层：**

| 字段 | 层 |
|---|---|
| segment start/end、anchor/fallback ts、OCR 调用次数 | L0 |
| Mock text / confidence | L0（NFC text） |
| final entries after dedupe | L0 |
| Vision | **不在 6.2 门禁** |

---

## 5. 建议 C++ API（方向）

```text
// detector.hpp / ocr.hpp
struct IDetector {
  virtual ~IDetector() = default;
  virtual std::optional<Region> detect(const Frame& frame) = 0;
};

struct IOcrEngine {
  virtual ~IOcrEngine() = default;
  virtual OcrResult recognize(const ImageView& image) = 0;
};

// pipeline.hpp
struct SegmentEvent {
  int64_t start_ms;
  int64_t end_ms;
  // 拥有或共享：实现可选——建议 SegmentEvent 持有 ImageBuffer 拷贝或
  // 共享_ptr<ImageBuffer>，避免 feed 返回后调用方帧被销毁。
  // **parity 契约：dump 记录 timestamp_ms 列表；像素由 fixture 可重建。**
  optional<Frame> anchor_frame;
  vector<Frame> fallback_frames;
};

class Pipeline {
public:
  Pipeline(IDetector& detector, IOcrEngine& ocr, Config config = {});
  optional<SegmentEvent> feed(const Frame& frame);
  SubtitleEntry ocr_segment(const SegmentEvent& event);
  vector<SubtitleEntry> finalize();
  void cancel();
  // optional: size_t processed_count() const;
};
```

### 帧所有权（P0 设计点）

Python 的 `Frame` 持有 PIL 图像引用；段内 `anchor_frames` / samples 保留引用。  
C++ 必须在 `feed` 开段/采样时 **拷贝或共享拥有** `ImageBuffer`，否则 `ocr_segment` 悬垂。

**契约：** `Pipeline` 内部对保留帧做 deep copy（或 `shared_ptr<ImageBuffer>` 引用计数）；`SegmentEvent` 对外给出可安全 OCR 的帧。

### 色域契约（与 6.1 衔接）

| 步骤 | Python | C++ 要求 |
|---|---|---|
| Extractor 输出 | RGB | `PixelFormat::RGB24` |
| feed 内 convert | `RGB2BGR` | 在 signature 前得到 BGR 字节序 |
| signature | `_to_gray` 用 `COLOR_RGB2GRAY` 读该数组 | **保持怪癖**：BGR 字节 + RGB2GRAY（6.1 已文档化） |

Dump 与 C++ 必须使用同一语义标签，避免一侧「修正」BGR2GRAY。

---

## 6. 工程落点

```text
cpp/include/sublift/
  detector.hpp
  ocr.hpp
  pipeline.hpp
cpp/src/core/
  pipeline.cpp
  mock_ocr.cpp          # 或 test_support
  fixed_detector.cpp
cpp/tests/
  pipeline_test.cpp
  parity/pipeline_parity_test.cpp
scripts/parity/
  dump_pipeline.py
  gen_pipeline_fixtures.py   # 可选
benchmark/parity/
  fixtures/pipeline/
  goldens/pipeline/pipeline.v1.json
```

- `Pipeline` 进 **`sublift_core`**（依赖 signature/changepoint/timeline/dedupe/line_select）。  
- OpenCV：仅 signature/SSIM 路径；feed 色域转换可用 OpenCV 或手写 swap。  
- Mock OCR **可不**链 OpenCV。

---

## 7. 6.2 完成定义

- [x] `feat-06201`–`feat-06205` 全部 `done` + evidence  
- [x] `feed`/`ocr_segment`/`finalize`/`cancel` 行为与冻结 golden 一致  
- [x] Mock 路径下 final entries L0；段代表帧 ts L0  
- [x] `ctest` 全绿；`dump_pipeline.py --check`；`./init.sh` 绿  
- [x] 产品默认仍 Python  
- [x] `feature-list` 6.2 块 done 或 covers 填齐  
- [x] 下一刀准备：**6.3** Extractor（`feat-063xx`）

## 8. 风险

| 风险 | 缓解 |
|---|---|
| 帧生命周期 | §5 所有权契约 + 单测悬垂场景 |
| 色域双边不一致 | dump 与 C++ 共享 pixel_semantics；禁止一侧修怪癖 |
| OCR 调度细节漂移 | 中间量 golden（调用次数、代表帧 ts、early_stop）优先于只比文本 |
| 范围膨胀到 Vision/ffmpeg | 硬边界 §2；接口留到 6.3/6.4 |
| line_select + 多帧共识浮点 | Mock 固定 conf；text L0 NFC |
| 非线程安全误用 | 头文件与 docs 明确串行 OCR |

## 9. 与相邻子阶段

| 已有（6.1） | 本阶段消费 |
|---|---|
| signature / changepoint / timeline / dedupe / line_select | Pipeline 内部 |
| Config 全字段 | 延迟锚、line_select 开关、merge 参数 |
| ImageBuffer / Frame / Region / OcrLine | 边界类型 |

| 后续 | 本阶段交付 |
|---|---|
| 6.3 Extractor | 可对 `Pipeline` 喂真实 `Frame` 流 |
| 6.4 Vision `IOcrEngine` | 替换 Mock |
| 6.5 Worker | 调 `feed`/`ocr_segment`/`finalize`/`cancel` |

---

## 10. 下一实现刀

**`feat-06201`** — 接口 + Mock OCR + FixedRegionDetector  

分支建议：`feat/cpp-6.2-interfaces`；提交前缀 `feat(phase6.2):`。
