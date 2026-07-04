# SubLift 架构设计

## 1. 设计原则

- **模块化、可插拔、低耦合高内聚**：各能力模块定义 Protocol，默认实现可替换
- **平台 API 隔离**：平台特定 API（Apple Vision）只能出现在 `ocr/vision.py`，核心层只依赖 Protocol
- **串联层不实现单一能力**：pipeline / export 组合能力模块，处理流程逻辑

## 2. 分层架构

| 层 | 模块 | 职责 |
|---|---|---|
| 能力模块 | `extractor` / `detector` / `ocr` | 单一能力，Protocol + 默认实现，可插拔 |
| 串联层 | `pipeline` / `export` | 组合能力模块，处理时间轴 / 去重 / 导出 |
| 入口层 | `cli` | 参数解析，调用 pipeline |
| 共享 | `models` / `config` | 核心数据模型与默认配置 |

## 3. 模块布局

```
src/sublift/
  __init__.py
  __main__.py              # python -m sublift 入口
  cli.py                   # CLI 入口（argparse）
  models.py                # 核心数据模型（frozen dataclass）
  config.py                # 默认配置 + SignatureConfig + ChangePointConfig
  pipeline/                # 串联层：打轴与编排
    signature.py           # 帧签名（前景占比 + dHash 双信号）
    changepoint.py         # 三态状态机（EMPTY⇄STABLE→STABLE'）
    timeline.py            # 时间轴构建（消费事件流）
    dedupe.py              # 去重合并（3-pass 纯函数）
    core.py                # Pipeline 类（端到端编排）
  detector/                # 能力模块：字幕区域检测
    base.py                # Detector Protocol
    bottom_crop.py         # 按比例裁剪下部（默认 30%）
    fixed_region.py        # 手动指定区域
  extractor/               # 能力模块：帧采样
    base.py                # Extractor Protocol
    ffmpeg_extractor.py    # ffmpeg/ffprobe subprocess 实现
  ocr/                     # 能力模块：OCR（可插拔多引擎）
    base.py                # OcrEngine Protocol
    mock.py                # MockOcrEngine（测试用）
    vision.py              # VisionOcrEngine（Apple Vision，PyObjC）
  export/                  # 串联层：字幕导出
    base.py                # Exporter Protocol（format + export 双方法）
    srt.py                 # SRT 实现
    ass.py                 # ASS 占位
    vtt.py                 # VTT 占位
```

## 4. 数据流

```
video
  └─[extractor]→ Frame(t, image)
      └─[detector, 首帧一次性]→ Region(box)
          └─[crop]→ 字幕带图像
              └─[signature]→ FrameSignature(fg_ratio, dhash)
                  └─[changepoint]→ StateEvent(IN/OUT/CHANGE)
                      └─[timeline]→ TimelineSegment(start_ms, end_ms)
                          └─[ocr, 每段一次]→ text
                              └─[dedupe]→ list[SubtitleEntry]
                                  └─[export]→ SRT 文件
```

详细设计见各模块文档：

- [pipeline 设计](design/pipeline.md) — 帧签名、状态机、时间轴、去重、编排
- [ocr 设计](design/ocr.md) — Protocol、Vision 实现、Mock、跨平台演进
- [extractor 设计](design/extractor.md) — Protocol、ffmpeg 实现、流式采样

## 5. 核心数据模型

`src/sublift/models.py` — 全部 `@dataclass(frozen=True)`，跨模块共享，避免循环依赖。

| 模型 | 字段 | 用途 |
|---|---|---|
| `BoundingBox` | `x, y, width, height: int` | 矩形区域，绝对像素坐标 |
| `Region` | `box: BoundingBox` | 字幕区域 |
| `Frame` | `timestamp_ms: int, image: PIL.Image` | 视频帧，附带时间戳 |
| `OcrResult` | `text: str, confidence: float` | OCR 识别结果 |
| `SubtitleEntry` | `start_ms, end_ms: int, text: str` | 字幕条目，pipeline 产出，export 消费 |

## 6. 抽象接口

四个 Protocol 均 `@runtime_checkable`，支持 `isinstance` 静态判定。

| Protocol | 方法 | 输入 → 输出 |
|---|---|---|
| `Extractor` | `extract` | `Path → Iterator[Frame]` |
| `Detector` | `detect` | `Frame → Region` |
| `OcrEngine` | `recognize` | `PIL.Image → OcrResult` |
| `Exporter` | `format` / `export` | `list[SubtitleEntry] → str` / `(entries, Path) → None` |

## 7. 技术栈

| 依赖 | 用途 | 性质 |
|---|---|---|
| Python 3.12+ | 语言 | 基线 |
| uv | 包管理 | 环境 |
| ffmpeg / ffprobe | 抽帧 + 探测 | 系统依赖（subprocess 调用） |
| Pillow | 图像中立表示 | 必需 |
| NumPy | 数组运算 | 必需 |
| opencv-python-headless | 自适应二值化、形态学、SSIM | 必需 |
| pyobjc-framework-Vision | Apple Vision OCR | macOS 可选 |
| pyobjc-framework-Quartz | CGImage/CGDataProvider | macOS 可选 |
| pytest / ruff / mypy | 测试 / lint / 类型检查 | dev |

## 8. 配置

`src/sublift/config.py` — `Config` dataclass，含 `SignatureConfig` 与 `ChangePointConfig` 嵌套配置。

| 参数 | 默认值 | 说明 |
|---|---|---|
| `sample_fps` | 5.0 | 帧采样率 |
| `region_bottom_ratio` | 0.3 | 字幕区域裁剪比例（下部 30%） |
| `confidence_threshold` | 0.5 | OCR 置信度阈值 |
| `merge_gap_ms` | 1000 | 去重合并间隔阈值 |
| `min_duration_ms` | 500 | 最小字幕时长 |

完整字段见 `src/sublift/config.py`，各参数说明见 [pipeline 设计](design/pipeline.md)。

## 9. 已知限制

- **dHash 对中文判别力不足**：9×8 降采样丢失汉字笔画高频信息，可能导致漏分段。详见 [HURDLES](HURDLES.md)。
- **OCR 锚帧过渡画面空文本**：锚帧可能落在字幕淡入/切换瞬间，Vision 识别不出文字。详见 [HURDLES](HURDLES.md)。
- **ASS/VTT 仅占位**：接口就位，实现待后续 Phase。
- **CLI 接入进行中**：`extract` 子命令尚未贯通，pipeline 可通过 Python API 调用。

端到端实测基线（Zootopia clip, 5fps）：23 段识别 21 段，召回/精确率 91.3%，0 误检。

## 10. 架构决策

完整决策记录见 [DECISIONS.md](DECISIONS.md)，要点：

- **ADR-0001**：分层架构 = 三能力模块 + 串联层 + 入口层
- **ADR-0002**：Apple Vision 经 PyObjC 桥接，不引入 Swift helper
- **ADR-0003**：打轴采用像素差异驱动（Architecture 2），OCR 后置
- **ADR-0004**：任务粒度合并为 10 个粗任务，subtasks 字段承载细节
