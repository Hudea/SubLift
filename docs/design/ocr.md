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

`OcrResult(text: str, confidence: float)` — `confidence` 为 0.0~1.0，多行文本以 `\n` 连接。

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
                              └─[obs.topCandidates_(1)]→ texts + confidences
                                  └─["\n".join(texts), mean(confidences)]→ OcrResult
```

### 关键约束：`setRecognitionLanguages_` 必须显式设置

`VNRecognizeTextRequest` 默认 `recognitionLanguages` 仅 `en-US`，**无法识别中文**。必须显式调用 `setRecognitionLanguages_(["zh-Hans", "en-US"])`。

默认识别语言：`DEFAULT_RECOGNITION_LANGUAGES = ["zh-Hans", "en-US"]`（简体中文 + 英文），覆盖 SubLift 核心场景。可通过 `recognition_languages` 参数覆盖。

### 多结果合并

`_collect_results` 遍历 `observations`，每个 observation 取 `topCandidates_(1)` 的最高置信候选：

- 文本：`"\n".join(texts)`（多行）
- 置信度：`sum(confidences) / len(confidences)`（均值）
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
| 固定 | `MockOcrEngine(text="你好", confidence=1.0)` | 每次 `recognize` 返回相同 `OcrResult` |
| 序列 | `MockOcrEngine(sequence=[OcrResult(...), ...])` | 按调用顺序返回，越界抛 `IndexError`（暴露测试 bug） |

序列模式用于 pipeline 闭环测试，模拟字幕逐段变化。

## 跨平台演进指引

新 OCR 引擎只需：

1. 在 `ocr/` 下新建实现文件（如 `ocr/paddle.py`）
2. 实现 `recognize(self, image: Image.Image) -> OcrResult` 方法
3. 满足 `OcrEngine` Protocol（`@runtime_checkable` 自动识别）

平台特定 API 只能出现在 `ocr/<platform>.py`，核心层（pipeline/cli）只依赖 `OcrEngine` Protocol，不导入具体实现。
