# Phase 6.4 — Vision OCR（ObjC++ adapter）

> 子阶段编码：`S = 4` → feature 前缀 `feat-064xx`  
> 任务跟踪：`docs/phases/phase6.json`  
> 总览：[phase6-overview.md](phase6-overview.md)  
> 契约：[architecture.md](architecture.md) · [parity-contract.md](parity-contract.md) · [engine-matrix-and-cutover.md](engine-matrix-and-cutover.md)  
> **门槛：** `feat-06301`–`feat-06305` 全部 `done`（已满足）  
> **Oracle：** `src/sublift/ocr/vision.py`（`VisionOcrEngine` + box 映射纯函数）

## 1. 目标

在 **不切换产品默认 runtime** 的前提下，于 **`sublift_vision_macos`** 落地 Apple Vision 的 **C++/ObjC++ adapter**，实现既有 `IOcrEngine`：

```text
ImageView (RGB24 优先)
    → CGImage (DeviceRGB, 8bpc, 24bpp, row bytes = w*3)
    → VNImageRequestHandler + VNRecognizeTextRequest
    → setRecognitionLanguages_(langs)   // 默认 zh-Hans, en-US
    → performRequests
    → topCandidates(1) → OcrLine{text, conf, box}
    → box: Vision 归一化(左下原点) → 像素 BoundingBox(左上原点) + clamp
    → sort (y, x)
    → OcrResult::from_lines
```

| 交付 | 说明 |
|---|---|
| `VisionOcrEngine` | 实现 `IOcrEngine::recognize`；仅 `.mm` / vision target |
| 坐标/clamp 纯函数 | **L0** 可单测、可 golden（不依赖 Vision 非确定性） |
| CMake 门控 | `SUBLIFT_ENABLE_VISION`（默认 OFF）；ON 时仅 **Apple** 链接 Vision/CoreGraphics/Foundation |
| 可用性探测 | `is_vision_available()` / 构造失败语义对齐 Python |
| Parity harness | 纯映射 L0 +（可选）live Vision 集成；**文本走 L4，质量水位走 L3** |

**本子阶段结束时：产品 CLI/GUI 仍走 Python。** 6.4 交付的是 **可注入 Pipeline 的真实 OCR 后端**，供 6.5 Worker 使用；**不做 cutover**（见引擎矩阵）。

## 2. 做 / 不做

### 做

1. 在 **`sublift_vision_macos`** 实现 `VisionOcrEngine : IOcrEngine`（ObjC++）。
2. **严格对齐** Python 默认识别语言 `["zh-Hans", "en-US"]`；构造可覆盖。
3. **不主动设置** `recognitionLevel` / revision 等 Python 未设置的属性（跟框架默认，禁止「顺手改 accurate」）。
4. 像素入口：`ImageView` **RGB24**（与 extractor / Pipeline crop 一致）；Gray/BGR 策略文档化（建议：非 RGB 时转换或明确错误，与 dump 一致）。
5. `perform` 失败或无候选 → 空 `OcrResult("", 0.0)`（与 Python 成功路径失败返回空一致；异常策略见 §7）。
6. 行序：`(box.y, box.x)` 稳定排序后再 `from_lines`。
7. **target 隔离：** `sublift_core` **零** ObjC/Vision 符号；Linux/`VISION=OFF` 不编译 `.mm` 实现体（stub 或 target 省略）。
8. macOS 集成测：合成图英文/中文（可 skip 无 Vision）；空白图空结果。
9. Parity：box 映射 L0 golden；live 文本 **L4**（允许微差）；GT 水位 **L3** 仅作可选/报告，**非 6.4 合并硬门**（硬门在 6.6 cutover）。

### 不做

| 项 | 归属 |
|---|---|
| UDS Worker / capability 上报 | **6.5** |
| 产品默认切 C++ Vision | **6.6** |
| Paddle 原生 | 引擎矩阵；**不在 6.4** |
| 全量 `timing_callback` / OcrCallDetail 复刻 | 6.4 **可选**；parity **不依赖** |
| 并行 `recognize` / 多线程 Vision | 禁止（与架构串行契约一致） |
| 改 line_select / confidence 阈值「适配」C++ | 禁止 |
| 要求 Vision 文本与某次 PyObjC 调用 **逐字 L0** | 禁止（L4） |

## 3. 任务一览

| ID | 名称 | 验收一句话 |
|---|---|---|
| **feat-06401** | 纯契约：归一化 box → 像素 + clamp + 排序约定 | 无 Vision framework 的表驱动 L0 单测 |
| **feat-06402** | CMake / availability 骨架 | `SUBLIFT_ENABLE_VISION=ON` 链 Vision；OFF/Linux 不破 core+ffmpeg+mock |
| **feat-06403** | `VisionOcrEngine::recognize` 主路径 | RGB24 → CGImage → request → lines/`from_lines`；失败空结果 |
| **feat-06404** | 语言配置 + macOS 集成烟测 | 默认 zh-Hans+en-US；中/英合成图；空白图；构造不可用语义 |
| **feat-06405** | Vision parity harness | `dump_vision.py`：box L0 golden + schema；live 可选 L4 记录；init 策略文档化 |

依赖：

```text
feat-06305 + feat-06201 (IOcrEngine)
    └── feat-06401 pure box/clamp (can live in vision or core-testable free functions)
            └── feat-06402 cmake + availability
                    └── feat-06403 recognize implementation
                            └── feat-06404 languages + integration
                                    └── feat-06405 golden / dump harness
```

**推荐一次一刀：** 06401 → 06402 → 06403 → 06404 → 06405。

---

## 4. 任务详述

### feat-06401 — 纯契约（无 Vision framework）

**Python oracle：** `_vision_box_to_pixel`、`_unpack_normalized_rect`、`_clamp_box`、`_collect_results` 中的排序与 `from_lines` 约定。

| 必须 | 说明 |
|---|---|
| API | 例如 `vision_normalized_box_to_pixel(nx,ny,nw,nh, w,h) → OcrCropBox` |
| 原点 | Vision：左下归一化；像素：左上；`y = round((1-ny-nh)*H)` 等与 Python **同一 round** |
| clamp | 与 Python `_clamp_box` 一致（含 w/h≤0 图像） |
| 排序键 | `(y, x)` 升序（文档 + 单测用假 lines） |
| 位置 | 优先 **纯 C++** 放 `sublift_vision_macos` 的 `.cpp` 或 `vision_geometry.hpp`，便于 Catch2 **不链** Vision.framework |
| 单测 | 表驱动：贴边、翻转 y、零尺寸、越界 clamp |

**不做：** `performRequests`、CGImage。

---

### feat-06402 — CMake 与可用性骨架

| 必须 | 说明 |
|---|---|
| Option | 沿用 `SUBLIFT_ENABLE_VISION`（默认 OFF）；ON ⇒ `APPLE` 否则 FATAL（已有） |
| Link | `Vision`、`CoreGraphics`、`Foundation`（按实现需要可加 `CoreVideo`/`AppKit`——**最小集**） |
| 符号 | `bool is_vision_available() noexcept`；OFF/stub → false |
| 头文件 | `cpp/include/sublift/vision.hpp`（或 `vision_ocr.hpp`）声明 `VisionOcrEngine` + defaults；实现仅 `.mm` |
| 依赖方向 | `sublift_vision_macos` → `sublift_core`；**core 不反链 vision** |
| 单测 | OFF 构建仍 全绿；ON 时至少 compile + `is_vision_available` 冒烟 |

**不做：** 完整 recognize（06403）。

---

### feat-06403 — `VisionOcrEngine::recognize`

**Python oracle：** `_recognize_untimed` 主路径（产品默认无 timing）。

| 必须 | 说明 |
|---|---|
| 输入 | `const ImageView&`；借用仅限调用期间 |
| 像素 | RGB24 → `CGImageCreate`（8/24、`bytesPerRow=width*3`、DeviceRGB）；语义对齐 `_pil_to_cgimage` |
| Request | `VNRecognizeTextRequest` + `recognitionLanguages`；**不**设置 Python 未设的 level |
| 结果 | `topCandidates(1)`；strip 空串跳过；box 失败时整图占位 box（同 Python） |
| 汇总 | `OcrResult::from_lines`（已有 C++） |
| 失败 | `perform` 不成功 → 空结果（不抛，对齐 untimed 路径） |
| 线程 | 文档：**非线程安全**；与 Pipeline 串行契约一致 |
| 内存 | `@autoreleasepool`（或等价）包住每次 recognize |

**不做：** timing 五阶段（可选钩子可后置）；Paddle。

---

### feat-06404 — 语言与集成烟测

| 必须 | 说明 |
|---|---|
| 默认语言 | `{"zh-Hans","en-US"}` 常量与 Python `DEFAULT_RECOGNITION_LANGUAGES` 一致 |
| 覆盖 | 构造参数传入自定义列表 |
| 不可用 | 无 framework / 编译 OFF 时：构造 throw 或 `expected` + 明确信息（对齐 Python `RuntimeError` 提示语义） |
| 集成 | macOS + VISION=ON：英文合成图、中文合成图（可 `CATCH` skip）；空白/近空白 → 空或低信息结果 |
| 标签 | `[vision][integration]`；CI 无 GUI/权限时 skip 策略写入 cpp/README |

**不做：** 全量 GT benchmark（6.6）。

---

### feat-06405 — Parity harness

| 必须 | 说明 |
|---|---|
| Dump | `scripts/parity/dump_vision.py` |
| L0 golden | 归一化 rect 表 → 期望 `OcrCropBox`；排序样例；**不**依赖 live Vision |
| Envelope | `oracle_commit`、`macos_version`、`vision_note`（机型/OS）；`golden_schema_version` |
| Live 可选 | `--live`：同图 Python Vision vs C++；记录 text 供 L4 人工/报告；**默认 CI 可只跑 L0** |
| C++ | `[parity][vision]` 加载 L0 golden |
| init.sh | L0 `--check` 接入；live 不强制进 init 默认路径 |

**比较层（钉死）：**

| 字段 | 层 |
|---|---|
| box 映射整数、排序键、空结果分支结构 | **L0** |
| Mock 已覆盖的 Pipeline 路径 | 仍 6.2 L0（不改） |
| Vision **文本 / conf** | **L4**（允许微差） |
| 固定 GT F1/CER… | **L3**（6.6 cutover 硬门；6.4 可选报告） |

---

## 5. 建议 C++ API（方向）

```text
// include/sublift/vision.hpp
namespace sublift {

inline constexpr std::string_view kDefaultVisionLanguages[] = {
  "zh-Hans", "en-US",
};

[[nodiscard]] OcrCropBox vision_normalized_box_to_pixel(
    double nx, double ny, double nw, double nh,
    std::int32_t image_width, std::int32_t image_height);

[[nodiscard]] bool is_vision_available() noexcept;

class VisionOcrEngine final : public IOcrEngine {
public:
  explicit VisionOcrEngine(
      std::vector<std::string> recognition_languages = {
          std::string{kDefaultVisionLanguages[0]},
          std::string{kDefaultVisionLanguages[1]},
      });

  [[nodiscard]] OcrResult recognize(const ImageView& image) override;
};

}  // namespace sublift
```

### 工程落点

```text
cpp/include/sublift/vision.hpp
cpp/src/vision_macos/
  CMakeLists.txt
  vision_geometry.cpp      # pure L0
  vision_ocr.mm            # VisionOcrEngine
  vision_availability.mm   # optional
cpp/tests/
  vision_geometry_test.cpp           # always
  vision_ocr_integration_test.mm.cpp # VISION=ON only
  parity/vision_parity_test.cpp      # L0 golden
scripts/parity/dump_vision.py
benchmark/parity/goldens/vision/vision_geometry.v1.json
```

- 删除/替换 `vision_stub.mm` 的「永远 false」为真实 availability（OFF 时 target 可不建）。  
- Worker 在 6.5 才 `link` 并实例化；6.4 可用单元/小可执行冒烟。

---

## 6. 与 Pipeline / Extractor 的衔接

```text
FfmpegExtractor → Frame RGB24
    → Pipeline::feed (BGR quirk 仅 signature)
    → ocr_segment → IOcrEngine::recognize(ImageView RGB crop)
                          ↑
                   VisionOcrEngine (6.4)
                   MockOcrEngine   (6.2)
```

- **坐标空间：** OCR 输入图已是 frame-local / ocr-crop；`OcrLine.box` 相对该图（既有 models 注释）。  
- ROI passthrough 后整图即 ROI；Vision box 仍相对传入 `ImageView` 的宽高。

---

## 7. 风险与语义钉扎

| 风险 | 处理 |
|---|---|
| Vision 文本非确定性 | 契约 **L4**；不把 live 文本当 init 硬失败 |
| PyObjC vs 原生 API 细微差别 | 语言列表、未设 level、topCandidates(1)、空串跳过、box 失败占位 **对齐** |
| CGImage 字节序/alpha | 与 Python RGB `tobytes` + bitmap info `0` 对齐；单测固定像素图 |
| 主线程 / 权限 | 文档化；集成测失败时 skip + note |
| ASan + ObjC | 6.4 不强制 ASan 全绿 vision；记录已知问题；6.6 cutover 再收紧 |
| 二进制膨胀 | Vision 仅进 `sublift_vision_macos` / 可选 worker |

---

## 8. 6.4 完成定义

- [ ] `feat-06401`–`feat-06405` 全部 `done` + evidence 写入 `phase6.json`
- [ ] 纯 box/clamp/排序 **L0** golden 绿；`dump_vision --check`（L0）接入约定路径
- [ ] macOS + `SUBLIFT_ENABLE_VISION=ON` 下 recognize 烟测绿（或文档化 skip 原因）
- [ ] `SUBLIFT_ENABLE_VISION=OFF` 与非 Apple 构建：core + ffmpeg + mock **仍绿**
- [ ] **产品默认仍 Python**；无 cutover
- [ ] `feature-list` 6.4 块 covers 填齐

---

## 9. 与后续子阶段

| 之后 | 关系 |
|---|---|
| **6.5 Worker** | C++ worker 按 capability 装配 `VisionOcrEngine` / Mock；UDS 协议已冻结 |
| **6.6 Cutover** | vision/mock → 默认 C++；GT **L3** + 运行时门；paddle 仍 Python |
| **Paddle native** | 另 feat，不阻塞 6.4–6.6 |
