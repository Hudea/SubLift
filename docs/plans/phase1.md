# Phase 1 设计与执行计划

> 本文件是 Phase 1 的设计源头与执行手册。任务跟踪见 `docs/phases/phase1.json`，
> 项目级功能块见 `feature-list.json`。架构细节待 `foundation.architecture` 任务
> 落入 `docs/ARCHITECTURE.md`，本文档在落地前为唯一设计参考。

## 1. 目标与范围

### 1.1 Phase 1 目标

交付一个**可运行的 CLI MVP**：一条命令把烧录字幕的本地视频还原为 SRT 文件。

```
uv run sublift extract <video> -o out.srt
```

### 1.2 在范围内

- 三能力模块的抽象接口与默认实现：`extractor`（帧采样）/`detector`（区域检测）/`ocr`（OCR）
- OCR 默认引擎：Apple Vision（通过 PyObjC 桥接）
- 端到端编排：帧采样 → 区域确定 → OCR → 时间轴生成 → 去重合并
- SRT 导出（ASS/VTT 仅留接口占位）
- `sublift extract` CLI 子命令
- 项目工具链：`uv` + `pyproject.toml` + ruff/mypy/pytest + `init.sh`

### 1.3 不在范围内

- GUI（Phase 2）
- 跨平台 Windows/Linux（Phase 3）
- PaddleOCR 第二引擎、引擎自动路由、对照模式（Phase 1 后期或后续）
- 配置文件 `sublift.toml`、进度展示（Phase 1 后期）
- ASS/VTT 完整实现（仅接口占位）

## 2. 模块布局

```
src/sublift/
  __init__.py
  __main__.py              # python -m sublift 入口
  cli.py                   # CLI 入口（argparse，零依赖起步）
  models.py                # 核心数据模型
  config.py                # 默认配置
  pipeline/                # 串联层
    __init__.py
    core.py                # 端到端编排
    timeline.py            # F5 时间轴生成（变化点状态机）
    dedupe.py              # F6 去重合并
  detector/                # 能力模块：字幕区域检测
    __init__.py
    base.py                # Detector Protocol + Region/BoundingBox
    bottom_crop.py         # 默认实现：裁剪下部 N%
  extractor/               # 能力模块：帧采样
    __init__.py
    base.py                # Extractor Protocol + Frame
    ffmpeg_extractor.py    # 实现：subprocess 调 ffmpeg
  ocr/                     # 能力模块：OCR（可插拔多引擎）
    __init__.py
    base.py                # OcrEngine Protocol + OcrResult
    mock.py                # MockOcrEngine（测试用）
    vision.py              # VisionOcrEngine（PyObjC）
  export/                  # 串联层：字幕导出
    __init__.py
    base.py                # Exporter Protocol
    srt.py                 # Phase 1 必需
    ass.py                 # 接口占位
    vtt.py                 # 接口占位
```

### 2.1 分层原则

- **能力模块**（detector/extractor/ocr）：只做单一能力，定义 Protocol + 默认实现，可插拔。
- **串联层**（pipeline/export）：组合能力模块，处理时间轴/去重/导出等流程逻辑。
- **入口层**（cli）：解析参数，调用 pipeline。
- 平台特定 API（Apple Vision）只能出现在 `ocr/vision.py`，核心层只依赖 Protocol。

## 3. 数据流

```
video
  └─[extractor]→ Frame(t, image)
      └─[detector]→ Region（Phase1: 一次性下部裁剪，非逐帧）
          └─[ocr]→ OcrResult(text, conf)
              └─[pipeline: timeline+dedupe]→ list[SubtitleEntry(start_ms,end_ms,text)]
                  └─[export]→ SRT 文件
```

### 3.1 阶段说明

| 阶段 | 模块 | 输入 | 输出 | Phase 1 策略 |
|---|---|---|---|---|
| 帧采样 | extractor | 视频路径 | `Iterator[Frame]` | ffmpeg 按 fps 抽帧（默认 1 fps） |
| 区域确定 | detector | 一帧/全部帧 | `Region` | 默认裁剪下部 30%，可手动指定 |
| OCR | ocr | 裁剪后图像 | `OcrResult` | Apple Vision，返回文本+置信度 |
| 时间轴 | pipeline.timeline | 逐帧 OcrResult | 带起止的条目 | 变化点状态机：出现→持续→消失 |
| 去重 | pipeline.dedupe | 带起止条目 | 合并后条目 | 合并连续相同/相似、消除抖动 |
| 导出 | export | 合并后条目 | 文件 | SRT |

## 4. 核心数据模型（`models.py`）

```python
@dataclass(frozen=True)
class BoundingBox:
    x: int
    y: int
    width: int
    height: int

@dataclass(frozen=True)
class Region:
    """字幕区域，绝对像素坐标。"""
    box: BoundingBox

@dataclass(frozen=True)
class Frame:
    timestamp_ms: int
    image: PIL.Image.Image   # 中立图像表示

@dataclass(frozen=True)
class OcrResult:
    text: str
    confidence: float        # 0.0 ~ 1.0

@dataclass(frozen=True)
class SubtitleEntry:
    start_ms: int
    end_ms: int
    text: str
```

## 5. 抽象接口（Protocol）

### 5.1 Extractor

```python
class Extractor(Protocol):
    def extract(self, video_path: Path) -> Iterator[Frame]: ...
```

### 5.2 Detector

```python
class Detector(Protocol):
    def detect(self, frame: Frame) -> Region: ...
```

### 5.3 OcrEngine

```python
class OcrEngine(Protocol):
    def recognize(self, image: PIL.Image.Image) -> OcrResult: ...
```

### 5.4 Exporter（串联层，同样 Protocol 化）

```python
class Exporter(Protocol):
    def export(self, entries: list[SubtitleEntry], output: Path) -> None: ...
```

## 6. 技术栈与依赖

| 依赖 | 用途 | 性质 | 依赖位置 |
|---|---|---|---|
| Python 3.12+ | 语言 | 基线 | pyproject.toml |
| uv | 包管理 | 基线 | 环境 |
| ffmpeg | 抽帧 | 系统依赖（subprocess 调用，不绑库） | extractor.ffmpeg |
| `pyobjc-framework-Vision` | Apple Vision | macOS 可选 | ocr.vision |
| `pyobjc-framework-Quartz` | CGImage/图像 | macOS 可选 | ocr.vision |
| `Pillow` | 图像中立表示 | 必需 | models / 多模块 |
| `pytest` | 测试 | dev | tests |
| `ruff` | lint | dev | 全项目 |
| `mypy` | 类型检查 | dev | 全项目 |

### 6.1 配置参数（Phase 1 默认值）

| 参数 | 默认值 | 说明 |
|---|---|---|
| `sample_fps` | 1.0 | 帧采样率 |
| `region` | 下部 30% | 字幕区域，可手动覆盖 |
| `confidence_threshold` | 0.5 | OCR 置信度阈值 |
| `merge_gap_ms` | 1000 | 合并间隔（去重） |
| `min_duration_ms` | 500 | 最小字幕时长 |

## 7. 任务拆分与执行顺序

任务粒度按「一个任务 = 一个可独立验收的功能块」划分，粗任务承载主验收，
内部细节通过 `subtasks` 字段承载（见 `docs/phases/phase1.json`）。
`description` 内嵌「验收：xxx」作为主验收标准。

### 7.1 foundation 基座（feat-001~003）

| id | 任务 | 依赖 | 前置/阻塞 | 分步验证 |
|---|---|---|---|---|
| `feat-001` | pyproject.toml 与 uv 初始化 | — | 前置：无 | `uv sync` 成功；`uv run python -c "import sublift"` 无错 |
| `feat-002` | 包结构骨架 | `feat-001` | 前置：feat-001 | `uv run python -m sublift` 退出 0；ruff/mypy 对空实现无错 |
| `feat-003` | init.sh 环境检查 | `feat-002` | 前置：feat-002 | `./init.sh` 退出 0；能检出 uv/ffmpeg/python |

### 7.2 三能力模块（extractor / detector / ocr）

| id | 任务 | 依赖 | 前置/阻塞 | 分步验证 |
|---|---|---|---|---|
| `feat-005` | 工具链配置细化 | `feat-001` | 前置：feat-001 | ruff/mypy/pytest 全绿；pytest 收集到至少 1 个测试 |
| `feat-006` | 帧采样模块（extractor） | `feat-002` | 前置：feat-002；阻塞项：依赖系统 ffmpeg | 合成短视频抽帧；帧数与时间戳断言正确 |
| `feat-007` | 字幕区域检测模块（detector） | `feat-002` | 前置：feat-002 | 给定帧尺寸断言裁剪区域正确 |
| `feat-008` | OCR 引擎模块（ocr） | `feat-002` | 前置：feat-002；阻塞项：Vision 仅 macOS、CI 不可用 | Mock 单测返回固定文本/置信度；macOS 真实帧识别出文本（可手动） |

### 7.3 串联层（pipeline / export）

| id | 任务 | 依赖 | 前置/阻塞 | 分步验证 |
|---|---|---|---|---|
| `feat-009` | 端到端编排与时间轴（pipeline） | `feat-006`, `feat-007`, `feat-008` | 前置：三能力模块 | Mock OCR + 合成帧跑通闭环，产出 SubtitleEntry 列表；timeline/dedupe 单测 |
| `feat-010` | 字幕导出（export） | `feat-009` | 前置：feat-009 | SRT 单测生成符合格式 SRT；ASS/VTT 占位 mypy 通过 |

### 7.4 文档收尾（feat-004，置末）

| id | 任务 | 依赖 | 前置/阻塞 | 分步验证 |
|---|---|---|---|---|
| `feat-004` | 项目文档与架构填充 | `feat-010` | 前置：feat-010（实现完成后文档才准确） | README 含 install/usage；ARCHITECTURE 含模块职责与数据流；AGENTS 验证命令段非空 |

### 7.5 CLI 与端到端验收

CLI `extract` 子命令的实现随 `feat-009`/`feat-010` 完成后接入（已在 `feat-002` 建好桩）。
真实视频端到端验收（`sublift extract <video> -o out.srt`）作为 **Phase 1 级验收门**，
不单列任务，见 §9。

**合计 10 个粗粒度任务（feat-001~010，其中 feat-004 置末）。**

## 8. 执行顺序总览

```
feat-001 → feat-002 → feat-003          （foundation 基座）
feat-005                                （工具链细化，可与后续并行）
  → feat-006（extractor）
  → feat-007（detector）
  → feat-008（ocr，含 Mock + Vision）
  → feat-009（pipeline，汇聚三能力模块）
  → feat-010（export）
  → CLI extract 接入 + Phase 1 端到端验收门
  → feat-004（文档收尾，实现稳定后再写）
```

## 9. Phase 1 验收标准

- [ ] `uv run sublift extract <video> -o out.srt` 可运行，真实 1080p 视频产出 SRT（端到端验收门）
- [ ] 三模块接口 + pipeline 逻辑有单元测试（Mock OCR + 合成帧，不依赖真实视频/Vision）
- [ ] `uv run pytest` / `uv run ruff check .` / `uv run mypy src` 全绿
- [ ] `./init.sh` 能完成环境检查与验证
- [ ] `feature-list.json` / `docs/phases/phase1.json` 中任务状态与证据已更新

## 10. 风险与备忘

| 风险 | 缓解 |
|---|---|
| Apple Vision 在 CI 环境不可用 | `ocr.vision` 集成测试可手动；pipeline 闭环用 `ocr.mock` 验证 |
| ffmpeg 系统依赖缺失 | `init.sh` 检出并提示；extractor.ffmpeg 测试用合成短视频 |
| PyObjC 在非 macOS 不可用 | 标记为 macOS 可选依赖；`ocr.vision` 导入失败时优雅降级提示 |
| Phase 1 范围蔓延 | 严格按 22 任务执行；增强项（F9–F13）显式排除 |
