# OCR 引擎设计

> 本文记录 `src/sublift/ocr/` 的历史 Python OCR 设计，用于迁移追溯与离线对照；
> PyObjC/RapidOCR 安装、自动下载与 Python CLI/GUI 描述都不是当前产品合同。
> Native OCR 边界以 [`docs/cpp/`](../cpp/README.md)、C++ Ports/Adapters 与测试为准。

## 模块职责

| 文件 | 职责 |
|---|---|
| `base.py` | `OcrEngine` Protocol 定义 |
| `mock.py` | `MockOcrEngine`，测试用，支持固定/序列双模式 |
| `vision.py` | `VisionOcrEngine`，Apple Vision 实现（PyObjC 桥接） |
| `paddle.py` | `PaddleOcrEngine`，PaddleOCR 实现（rapidocr PP-OCRv6，跨平台） |

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

## PaddleOcrEngine

通过 rapidocr 桥接 PP-OCRv6 的跨平台 OCR 第二引擎（feat-05001）。

### 依赖与安装

```bash
uv sync --extra paddle   # rapidocr>=3.9.0,<4.0.0 + onnxruntime>=1.16
```

首次运行自动下载模型到 `~/.cache/sublift/rapidocr-models`（约 30MB），跨 venv 复用。离线环境可预下载：

```bash
uv run --extra paddle python -c "from sublift.ocr import PaddleOcrEngine; PaddleOcrEngine()"
```

> 通过 `PaddleOcrEngine()` 构造才能把模型写入
> `~/.cache/sublift/rapidocr-models`；不要使用不带该缓存配置的 RapidOCR 下载命令。

### 模型规格

PP-OCRv6 三规格：`tiny` / `small`（默认）/ `medium`。通过 `model_type` 参数切换：

```python
engine = PaddleOcrEngine(model_type="medium")  # 中文漏字多时升级
```

### 实现要点

- 优雅降级：模块级 `_PADDLE_AVAILABLE` 标志，不可用时实例化抛 `RuntimeError`
- 输入约定：直接传 `PIL.Image` 给 RapidOCR（由其对 PIL 来源做 RGB->BGR 转换）；
  切勿传 RGB ndarray（RapidOCR 按 BGR 消费 ndarray，致彩色字幕 R/B 颠倒）
- 四角点转包围盒：rapidocr `result.boxes` shape `(N, 4, 2)` → axis-aligned `BoundingBox`
- 行级输出：与 Vision 一致，按 `(box.y, box.x)` 排序，`OcrResult.from_lines` 汇总
- 故障语义：仅「无识别结果」（`result.txts is None`）返回空 `OcrResult`；
  运行时故障（推理 / 模型加载 / box 映射）向上传播，由 bridge 转为 `done(ok=False)`，
  不伪装成空字幕
- 不接 Phase 4.2 归因：`PaddleOcrEngine` 不实现 `timing_callback`（契约允许）

## 跨平台演进指引

新 OCR 引擎只需（参考已实现的 `paddle.py`）：

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
    script="auto",  # cjk | latin | auto
    center_x, center_y, height, y_min, y_max  # crop 像素坐标
)
```

| 来源 | 行为 |
|---|---|
| GUI | 用户选中候选框并集 → crop 相对 profile；按候选文字推断 cjk/latin/auto，经 `start_job.subtitle_profile` 传入 |
| region_box 无 profile | bridge/benchmark 用 `from_crop(w,h)` 整带默认；默认 auto，可显式固定 script |
| CLI 无 region | Pipeline 在 detector 确定 Region 后 `from_crop`；`--script` 可覆盖默认 auto |

**不改变** 全宽 `region_box` 裁剪；profile 只描述「在 crop 内选哪一带」。

### 行级选择与共识（feat-034c/d）

```
OcrResult.lines
  → select_line(profile)          # script / Y / 字号 / 中心
  → 多帧 samples
  → consensus_text（相似邻域 / medoid / 真实簇票数）
  → cleanup_subtitle_text         # CJK 粘连横幅边缘 + 常见标点
  → should_accept_text            # 高 conf 或 低 conf+多帧稳定
```

cleanup 不删除纯英文、空格分隔英文或夹在中文内部的英文（如 `ZPD警局`）；
只有显式 CJK 画像下与中文边界直接粘连的拉丁横幅才会被清理。

Config：`enable_line_select=True`，`subtitle_script="auto"`，`low_conf_threshold=0.28`，`ocr_consensus_frames=4`。
`enable_line_select=False` 回退整区 join + 全局阈值。

## 性能归因（Phase 4.2 计划）

当前 Pipeline 已记录一次 `OcrEngine.recognize()` 的总 wall；下一步只为 Vision 增加可选的
内部计时能力，不改变 `OcrEngine` Protocol、识别语言、`OcrResult` 或业务结果。内部树会区分
PIL/CGImage 输入准备、request 设置、`performRequests`、observation 映射与 residual；这些
数字是 `ocr` parent 的解释，不会与 parent 一起计入 core coverage。`trace` 还会记录匿名
输入尺寸、每段最多 4 次调用和早停原因，不记录文本或图片。详细契约见
[OCR 内部性能归因设计](ocr-performance-attribution.md)。
