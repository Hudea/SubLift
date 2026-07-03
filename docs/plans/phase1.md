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

执行顺序即依赖链。每个任务对应 `docs/phases/phase1.json` 的一条记录，
`description` 内嵌「验收：xxx」作为分步验证标准，`evidence` 待完成后填写实际命令与结果。

### 7.1 foundation（项目骨架与工具链）

| id | 任务 | 依赖 | 前置/阻塞 | 分步验证 |
|---|---|---|---|---|
| `feat-001` | pyproject.toml 与 uv 初始化 | — | 前置：无 | `uv sync` 成功；`uv run python -c "import sublift"` 无错 |
| `feat-002` | 包结构骨架 | `feat-001` | 前置：feat-001 | `uv run python -m sublift` 退出 0；ruff/mypy 对空实现无错 |
| `feat-003` | init.sh 环境检查 | `feat-002` | 前置：feat-002 | `./init.sh` 退出 0；能检出 uv/ffmpeg/python |
| `feat-004` | README.md | `feat-002` | 前置：feat-002 | 文档存在；含 install/usage 段 |
| `feat-005` | ruff/mypy/pytest 配置 | `feat-001` | 前置：feat-001 | `uv run ruff check .`、`uv run mypy src`、`uv run pytest` 全绿 |
| `feat-006` | ARCHITECTURE.md 填充与 AGENTS.md 验证命令段 | — | 前置：无（文档任务） | ARCHITECTURE.md 含模块职责与数据流；AGENTS.md 验证命令段非空 |

### 7.2 extractor（帧采样）

| id | 任务 | 依赖 | 前置/阻塞 | 分步验证 |
|---|---|---|---|---|
| `feat-007` | Extractor Protocol 与 Frame 模型 | `feat-002` | 前置：feat-002 | mypy 通过；单测可实例化一个最小 Extractor |
| `feat-008` | ffmpeg 抽帧实现 | `feat-007` | 前置：feat-007；阻塞项：依赖系统 ffmpeg | 合成短视频抽帧；帧数与时间戳断言正确 |

### 7.3 detector（字幕区域检测）

| id | 任务 | 依赖 | 前置/阻塞 | 分步验证 |
|---|---|---|---|---|
| `feat-009` | Detector Protocol 与 Region 类型 | `feat-002` | 前置：feat-002 | mypy 通过；单测 |
| `feat-010` | 下部裁剪默认实现 | `feat-009` | 前置：feat-009 | 给定帧尺寸断言裁剪区域正确 |

### 7.4 ocr（OCR 引擎）

| id | 任务 | 依赖 | 前置/阻塞 | 分步验证 |
|---|---|---|---|---|
| `feat-011` | OcrEngine Protocol 与 OcrResult 类型 | `feat-002` | 前置：feat-002 | mypy 通过；单测 |
| `feat-012` | MockOcrEngine（测试用） | `feat-011` | 前置：feat-011 | 单测返回固定文本/置信度 |
| `feat-013` | VisionOcrEngine（Apple Vision） | `feat-011` | 前置：feat-011；阻塞项：仅 macOS 可用、CI 环境不可用 | macOS 上真实帧识别出文本（集成测试，可手动；CI 跳过） |

### 7.5 pipeline（端到端编排）

| id | 任务 | 依赖 | 前置/阻塞 | 分步验证 |
|---|---|---|---|---|
| `feat-014` | SubtitleEntry 核心模型 | `feat-002` | 前置：feat-002 | 单测覆盖构造与字段 |
| `feat-015` | 端到端编排 | `feat-007`, `feat-009`, `feat-012`, `feat-014` | 前置：四依赖 | Mock OCR + 合成帧跑通闭环，产出 `SubtitleEntry` 列表 |
| `feat-016` | 变化点状态机（F5 时间轴） | `feat-014` | 前置：feat-014 | 单测覆盖出现/持续/消失/合并场景 |
| `feat-017` | 去重合并（F6） | `feat-014` | 前置：feat-014 | 单测覆盖连续相同/相似/抖动场景 |

### 7.6 export（字幕导出）

| id | 任务 | 依赖 | 前置/阻塞 | 分步验证 |
|---|---|---|---|---|
| `feat-018` | Exporter Protocol | `feat-002` | 前置：feat-002 | mypy 通过 |
| `feat-019` | SRT exporter | `feat-018`, `feat-014` | 前置：feat-018、feat-014 | 单测生成符合格式 SRT；时间码/序号正确 |
| `feat-020` | ASS/VTT 接口占位 | `feat-018` | 前置：feat-018 | mypy 通过（`NotImplemented`） |

### 7.7 cli（CLI 入口）

| id | 任务 | 依赖 | 前置/阻塞 | 分步验证 |
|---|---|---|---|---|
| `feat-021` | CLI extract 子命令 | `feat-015`, `feat-019` | 前置：feat-015、feat-019 | `uv run sublift extract --help` 正常；参数解析单测 |
| `feat-022` | 端到端真实视频验收 | `feat-021`, `feat-013` | 前置：feat-021、feat-013；阻塞项：依赖真实视频样本与 macOS Vision 环境 | `uv run sublift extract <video> -o out.srt` 产出可加载 SRT |

**合计 22 个细粒度任务。**

## 8. 执行顺序总览

```
feat-001..006（foundation，6 个）
  → feat-007 → feat-008                （extractor）
  → feat-009 → feat-010                （detector）
  → feat-011 → feat-012                （ocr: base + mock）
              → feat-013               （ocr: vision，可与 pipeline 并行）
  → feat-014                           （pipeline models）
  → feat-015（汇聚 feat-007/009/012/014）
  → feat-016 → feat-017                （timeline + dedupe）
  → feat-018 → feat-019 → feat-020     （export）
  → feat-021 → feat-022                （cli + e2e）
```

## 9. Phase 1 验收标准

- [ ] `uv run sublift extract <video> -o out.srt` 可运行，真实 1080p 视频产出 SRT
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
