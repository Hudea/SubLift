# OCR 引擎设计

> `src/sublift/ocr/` — OCR 抽象接口与实现。平台特定 API 唯一容身处（ADR-0002）。

## 模块职责

| 文件 | 职责 |
|---|---|
| `base.py` | `OcrEngine` Protocol 定义 |
| `mock.py` | `MockOcrEngine`，测试用，支持固定/序列双模式 |
| `vision.py` | `VisionOcrEngine`，Apple Vision 实现（PyObjC 桥接） |

## OcrEngine Protocol

```python
@runtime_checkable
class OcrEngine(Protocol):
    def recognize(self, image: Image.Image) -> OcrResult: ...
```

`@runtime_checkable` 支持 `isinstance` 静态判定，用于 Pipeline 注入校验。

### 数据模型

```python
@dataclass(frozen=True)
class OcrLine:
    text: str
    confidence: float          # 0.0~1.0，该行置信度
    box: BoundingBox           # 相对 recognize 输入图，像素，原点左上

@dataclass(frozen=True)
class OcrResult:
    text: str                  # 兼容汇总：多行 "\n".join
    confidence: float          # 兼容汇总：各行 conf 均值
    lines: tuple[OcrLine, ...] = ()

    @staticmethod
    def from_lines(lines: Sequence[OcrLine]) -> OcrResult: ...
```

| 约定 | 说明 |
|---|---|
| 行级语义 | 以 `lines` 为准；引擎**不**在内部做业务选行 |
| 兼容 join | `OcrResult.from_lines`：`text="\n".join(...)`，`confidence=mean`；空 → `("", 0.0, ())` |
| 旧构造 | `OcrResult(text, confidence)` 仍合法，`lines=()` |
| 阈值 | 引擎**不**按 `confidence_threshold` 丢行；过滤在 pipeline / 行级选择器 |

## VisionOcrEngine

通过 PyObjC 桥接 `VNRecognizeTextRequest`，是 macOS 平台特定 API 唯一出现的位置。

### 识别流程

```
PIL.Image
  └─[convert("RGB") + tobytes()]→ raw_bytes
      └─[CGDataProviderCreateWithData + CGImageCreate]→ CGImage
          └─[VNImageRequestHandler.initWithCGImage_options_]→ handler
              └─[VNRecognizeTextRequest.alloc().init()]→ request
                  └─[setRecognitionLanguages_(langs)]→ 设置识别语言
                      └─[handler.performRequests_error_([request], None)]→ 执行
                          └─[request.results()]→ observations
                              └─[obs.topCandidates_(1) + boundingBox]→ OcrLine[]
                                  └─[按 box.y, box.x 排序]→ OcrResult.from_lines
```

### 关键约束：`setRecognitionLanguages_` 必须显式设置

`VNRecognizeTextRequest` 默认 `recognitionLanguages` 仅 `en-US`，**无法识别中文**。必须显式调用 `setRecognitionLanguages_(["zh-Hans", "en-US"])`。

默认识别语言：`DEFAULT_RECOGNITION_LANGUAGES = ["zh-Hans", "en-US"]`（简体中文 + 英文），覆盖 SubLift 核心场景。可通过 `recognition_languages` 参数覆盖。

### 行级收集与 box 坐标系

`_collect_results` 遍历 `observations`，每个 observation 取 `topCandidates_(1)`：

- 跳过无 candidate 或空白 text
- `boundingBox()`：Vision 归一化、**原点左下** → `_vision_box_to_pixel` 转为像素、**原点左上**、相对输入图
- 按 `(box.y, box.x)` 升序稳定行序
- `OcrResult.from_lines(lines)` 生成兼容 `text`/`confidence`
- 无结果：`OcrResult("", 0.0)`

### 优雅降级

模块级 `try/except ImportError` 设置 `_VISION_AVAILABLE` 标志：

- PyObjC 未安装时，模块加载不报错，`_VISION_AVAILABLE = False`
- `VisionOcrEngine.__init__` 检查标志，不可用时抛 `RuntimeError`，提示 `uv sync --extra vision`
- `is_vision_available()` 函数供外部查询

这样设计使得：未装 vision 依赖时，`import sublift.ocr.vision` 不报错，pipeline 可用 MockOcrEngine 跑闭环测试。

## MockOcrEngine

测试用，支持两种返回模式：

| 模式 | 初始化 | 行为 |
|---|---|---|
| 固定 | `MockOcrEngine(text="你好", confidence=1.0)` | 每次 `recognize` 返回相同 `OcrResult`，`lines=()` |
| 固定多行 | `MockOcrEngine(lines=[OcrLine(...), ...])` | `from_lines` 填充 text/conf/lines |
| 序列 | `MockOcrEngine(sequence=[OcrResult(...), ...])` | 按调用顺序返回，越界抛 `IndexError`（暴露测试 bug） |

序列模式用于 pipeline 闭环测试，模拟字幕逐段变化。

## 跨平台演进指引

新 OCR 引擎只需：

1. 在 `ocr/` 下新建实现文件（如 `ocr/paddle.py`）
2. 实现 `recognize(self, image: Image.Image) -> OcrResult` 方法
3. 优先填充 `lines`，用 `OcrResult.from_lines` 生成兼容字段
4. 满足 `OcrEngine` Protocol（`@runtime_checkable` 自动识别）

平台特定 API 只能出现在 `ocr/<platform>.py`，核心层（pipeline/cli）只依赖 `OcrEngine` Protocol，不导入具体实现。

## 与 feat-034 的关系

| 子任务 | 职责 |
|---|---|
| **034a** | 引擎吐出 `OcrLine`；兼容 join |
| **034b** | `SubtitleProfile` + GUI/IPC 透传；几何相对 crop |
| **034c** | `pipeline/line_select.py` 逐行评分与低置信放行 |
| **034d** | 段内多代表帧共识 |
| **034e** | benchmark 过门 |

### SubtitleProfile（feat-034b）

```python
SubtitleProfile(
    script="cjk",   # cjk | latin | auto
    center_x, center_y, height, y_min, y_max  # crop 像素坐标
)
```

| 来源 | 行为 |
|---|---|
| GUI | 用户选中候选框并集 → crop 相对 profile，经 `start_job.subtitle_profile` 传入 |
| region_box 无 profile | bridge/benchmark 用 `from_crop(w,h)` 整带默认 |
| CLI 无 region | Pipeline 在 detector 确定 Region 后 `from_crop` |

**不改变** 全宽 `region_box` 裁剪；profile 只描述「在 crop 内选哪一带」。

### 行级选择与共识（feat-034c/d）

```
OcrResult.lines
  → select_line(profile)          # script / Y / 字号 / 中心
  → cleanup_subtitle_text         # 去 PHISON 尾巴、统一 …「」
  → 多帧 samples
  → consensus_text（多数 / medoid）
  → should_accept_text            # 高 conf 或 低 conf+多帧稳定
```

Config：`enable_line_select=True`，`low_conf_threshold=0.28`，`ocr_consensus_frames=4`。  
`enable_line_select=False` 回退整区 join + 全局阈值。
