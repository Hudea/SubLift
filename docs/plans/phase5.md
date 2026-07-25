# Phase 5.0 - PaddleOCR 引擎接入

> 本文件是 Phase 5.0 的设计源头与执行手册。任务跟踪见 `docs/phases/phase5.json`，
> 项目级功能块见 `feature-list.json` 的 `phases[phase5]` 块。
>
> 编号规则变更：自 Phase 5 起采用「阶段 + 序号」编码——`05`（Phase 5）+ `0`（小阶段 5.0）
> + `001`（feature 序号）= `feat-05001`。Phase 1-4 的旧编号 `feat-001~043` 保持不动，两套并存。

## 1. 主题、目标与为什么现在做

**主题：** 接入 PaddleOCR 作为第二个 OCR 引擎，与 Apple Vision 并列可选。这是项目自 Phase 1
起就在需求（F14）与各 Phase plan 中显式预留、反复推迟的「第二引擎」，现在落地。

**目标：** 用户通过 `--engine paddle`（CLI）或设置面板选择 PaddleOCR（GUI）即可完成与 Vision
等价的字幕提取流程；核心层（pipeline / bridge）零修改，`OcrEngine` Protocol 不变。

**为什么现在做：**

- Phase 4.2（feat-043）已完成 Vision 内部性能归因，给出可信 baseline，并在设计契约中明确
  「Mock/Paddle 等未实现 observer 的引擎不因归因而失效」——为第二引擎接入扫清了协议障碍。
- Phase 1-4 plan 均将 PaddleOCR 列为「不在范围 / 后续 Phase」，`REQUIREMENTS.md` F14 状态为
  「❌ 接口已预留，未接入」。Phase 5 推进 F14 → ✅。
- `design/ocr.md` §跨平台演进指引早已给出 `ocr/paddle.py` 接入模板。

**这不是什么：** 不是跨平台抽象层、不是引擎自动路由、不是多引擎对照模式、不是 PaddleOCR
内部归因。这些显式排除（见 §2 不做）。

## 2. 范围

### 做

- 新增 `src/sublift/ocr/paddle.py`：`PaddleOcrEngine` + `is_paddle_available()`，实现
  `OcrEngine` Protocol，产出带 `lines` 的 `OcrResult`（`from_lines`）。
- 依赖：`rapidocr>=3.9.0` + `onnxruntime`（onnxruntime 后端，无 paddlepaddle 依赖），
  PP-OCRv6 small 默认模型，作为 `paddle` optional-dep group。
- 模型缓存：覆盖 rapidocr 默认的 site-packages 落点为 `~/.cache/sublift/rapidocr-models`，
  跨 venv 复用。
- 接线：CLI `--engine paddle`、IPC server `--engine paddle`、GUI `OcrEngineName.paddle`
  + 两处 Picker。
- 测试：单测 + 降级测试 + 集成测试（skipif 无 rapidocr），复用现有 `_make_text_image`。
- 文档：`design/ocr.md` 加 PaddleOcrEngine 章节、`README.md` 加安装/用法、`REQUIREMENTS.md`
  F14 状态推进、`DECISIONS.md` 增 ADR（引擎选型 + 模型缓存策略 + 编号规则变更）。

### 不做

- **不接 Phase 4.2 归因系统**：`PaddleOcrEngine` 不实现 `timing_callback`。Phase 4.2 设计
  契约（`design/ocr-performance-attribution.md:98`）明确允许「Mock/Paddle 等未实现 observer」。
  PaddleOCR 的 det+rec 两阶段与 Vision 五阶段语义不同，强行映射会扭曲归因含义。
- 不做引擎自动路由（按语言/质量自动选引擎）。
- 不做多引擎对照模式（Vision vs PaddleOCR 并行输出对比）。
- 不做跨平台抽象层（引擎选择仍由用户显式指定）。
- 不做 PaddleOCR benchmark 报告（如需对照基线，另开 Phase 5.1+）。
- 不改 `OcrEngine` Protocol、不改 pipeline / bridge 核心、不改 `OcrResult`/`OcrLine` 模型。
- 不做 GUI 首次下载模型的进度提示 UI（仅在文档与 CLI 输出提示，见 §7）。

## 3. 现状基线（main = Phase 4.2）

接入前必须基于以下 main 真实状态设计（与 Phase 2 基点不同）：

- `models.py`：`OcrLine(text, confidence, box)` + `OcrResult.text/confidence/lines` +
  `OcrResult.from_lines()`。引擎**应优先填充 `lines`**，`text`/`confidence` 由 `from_lines` 生成。
  另有 `SubtitleProfile`（feat-034 行级选择），引擎无需关心。
- `ocr/base.py`：`OcrEngine` Protocol 签名 `recognize(image) -> OcrResult`，**未变**。
- `ocr/vision.py`：已加 `timing_callback`（Phase 4.2 归因），构造签名
  `(recognition_languages=None, *, timing_callback=None)`。PaddleOcrEngine 不跟进此参数。
- `ocr/__init__.py`：导出 `MockOcrEngine` / `VisionOcrEngine` / `is_vision_available`。
- `cli.py`：`--engine` choices=`[vision, mock]`，`_build_ocr_engine` 分支构造。
- `ipc/server.py`：`--engine` choices=`[vision, mock]`，`main()` 按 engine 选 factory。
- `ipc/bridge.py`：`BridgeHandler(ocr_engine_factory, ocr_engine_kwargs)`，构造调
  `self._ocr_engine_factory(**self._ocr_engine_kwargs)`。**接入点不变**——paddle factory 透传即可。
  bridge 现传 `subtitle_profile` 给 Pipeline，与引擎无关。
- GUI `Messages.swift`：`OcrEngineName` enum = `{vision, mock}`，注释「paddle 留待 Phase 3」。
- GUI `SettingsView.swift` + `SubLiftMacApp.swift`：两处 Picker（vision/mock）。
  `PipelineClient.start(engine:)` 透传任意 String 给 `--engine`，无需改。
- `design/ocr.md` §跨平台演进指引：已给 `ocr/paddle.py` 模板（新建文件 + 实现 recognize + 满足 Protocol）。
- `REQUIREMENTS.md` F14：PaddleOCR 第二引擎，❌ 接口已预留未接入。
- `DECISIONS.md`：无 PaddleOCR 相关 ADR。

## 4. 依赖与模型

### 4.1 依赖栈选型（ADR 待落）

选用 `rapidocr>=3.9.0`（统一主线包）+ `onnxruntime`，**不选** `rapidocr-onnxruntime` 1.x
（后者停在 PP-OCRv4，已基本停更）。理由：

- rapidocr 3.9.0 起默认 PP-OCRv6 det+rec small，中文（zh-Hans）精度优于 v4。
- onnxruntime 后端，无 paddlepaddle 重依赖，pip 直装，几十 MB，跨平台。
- 符合项目「通用可选依赖」定位（AGENTS.md 技术栈：PaddleOCR 通用可选依赖）。

PP-OCRv6 三规格（tiny / small / medium），small 为库默认，字幕场景起步用 small；
引擎暴露 `model_type` 参数，实测中文漏字多可升 medium。不影响 Protocol。

### 4.2 rapidocr 3.x API（源码 `utils/output.py` 核实）

```python
from rapidocr import RapidOCR
engine = RapidOCR(params={...})        # 构造一次复用（加载模型）
result = engine(img_ndarray)           # img: ndarray / path / bytes
# result 是 RapidOCROutput dataclass：
#   result.txts   : tuple[str, ...] | None
#   result.scores : tuple[float, ...] | None
#   result.boxes  : np.ndarray | None   shape (N, 4, 2)，4 个角点
# 无识别结果时 txts/scores/boxes 均为 None
```

`result.boxes` 是四角点（非矩形），需转 `BoundingBox`：取 4 点的 axis-aligned 包围盒
`(min_x, min_y, max_x-min_x, max_y-min_y)`，clamp 到图像范围。

### 4.3 模型缓存策略（ADR 待落）

rapidocr 默认把模型下到 **site-packages 内 `rapidocr/models/`**（源码 `main.py:34,59-60`），
venv 重建即丢失，不理想。本引擎覆盖为用户缓存目录：

- 默认 `~/.cache/sublift/rapidocr-models`，通过构造参数 `Global.model_root_dir` 指定。
- 跨 venv 复用、不污染 site-packages、符合 macOS 缓存惯例。
- 首次 `RapidOCR()` 自动下载（惰性，SHA256 校验）：`PP-OCRv6_det_small.onnx` +
  `ch_PP-LCNet_x1_0_textline_ori_cls_server.onnx`（cls）+ `PP-OCRv6_rec_small.onnx` +
  `ppocr_keys_v1.txt`（字典），约几十 MB，之后复用。
- 离线环境可用 `python -m rapidocr download_models` 预下载到 `model_root_dir`（文档说明）。

## 5. 任务拆分

> 编号规则：`feat-05<小阶段><序号>`，本 Phase 为 5.0，故 `feat-05001` 起。
> 任务粒度按 ADR-0004：粗任务承载主验收，subtasks 承载细节。

### 5.1 任务列表

| id | 任务 | 依赖 | 验收 |
|---|---|---|---|
| **feat-05001** | PaddleOcrEngine 引擎实现 + 依赖 | - | `paddle.py` 实现 `OcrEngine` Protocol；`is_paddle_available()`；不可用抛 RuntimeError；`recognize` 产出带 lines 的 OcrResult；`uv sync --extra paddle` 成功；ruff/mypy 干净 |
| **feat-05002** | CLI + IPC server 接线 | feat-05001 | `--engine paddle` 在 cli.py / server.py 可选；不可用提示 `uv sync --extra paddle`；server factory 透传 PaddleOcrEngine；`uv run sublift extract <视频> --engine paddle -o out.srt` 跑通 |
| **feat-05003** | GUI 引擎选择接线 | feat-05001 | `OcrEngineName.paddle`；SettingsView + SubLiftMacApp 两处 Picker 加选项；`swift build` 成功；GUI 可选 PaddleOCR 跑通提取 |
| **feat-05004** | 测试 + 文档收尾 | feat-05002, feat-05003 | `tests/test_ocr.py` 加 PaddleOcrEngine 单测/降级/集成（skipif 无 rapidocr）；`design/ocr.md` + `README.md` + `REQUIREMENTS.md` F14 + `DECISIONS.md` ADR 更新；`./init.sh` 全绿 |

### 5.2 执行顺序

```
feat-05001 (引擎实现 + 依赖) ─┬─ feat-05002 (CLI + IPC server)
                              └─ feat-05003 (GUI)
                                        │
                                        └─ feat-05004 (测试 + 文档收尾)
```

### 5.3 feat-05001 子任务

| 子任务 | 交付物 | 停止条件 |
|---|---|---|
| 依赖接入 | `pyproject.toml` 加 `paddle` optional-dep + mypy override；`uv sync --extra paddle` | import rapidocr 成功，模型落到 `~/.cache/sublift/rapidocr-models` |
| 引擎实现 | `src/sublift/ocr/paddle.py`：`_PADDLE_AVAILABLE` + `is_paddle_available()` + `PaddleOcrEngine` | 满足 `OcrEngine` Protocol（`isinstance` 通过）；不可用抛 RuntimeError |
| 导出 | `ocr/__init__.py` 导出 `PaddleOcrEngine` / `is_paddle_available` | `from sublift.ocr import PaddleOcrEngine` 可用 |

## 6. 接入点明细

### 6.1 `pyproject.toml`

- `[project.optional-dependencies]` 加 `paddle = ["rapidocr>=3.9.0", "onnxruntime>=1.16"]`
- `[[tool.mypy.overrides]]` 加 `module = ["rapidocr.*", "rapidocr_onnxruntime.*"]` →
  `ignore_missing_imports = true`（无 stub）

### 6.2 `src/sublift/ocr/paddle.py`（新建）

模仿 `vision.py` 优雅降级模式：

- 模块级 `try/except ImportError` 设 `_PADDLE_AVAILABLE` / `_IMPORT_ERROR`
- `is_paddle_available() -> bool`
- `DEFAULT_MODEL_DIR = Path.home() / ".cache" / "sublift" / "rapidocr-models"`
- `PaddleOcrEngine.__init__(self, model_type: str = "small", model_root_dir: Path | str | None = None)`:
  - 不可用抛 `RuntimeError`（提示 `uv sync --extra paddle`）
  - `root = Path(model_root_dir) if model_root_dir else DEFAULT_MODEL_DIR`
  - 构造 `RapidOCR(params={"Global.model_root_dir": str(root), "Det.model_type": model_type,
    "Rec.model_type": model_type})` 存为实例属性
- `recognize(image) -> OcrResult`:
  - `np.array(image.convert("RGB"))` 转 ndarray
  - `result = self._engine(arr)`
  - `result.txts is None` → `OcrResult.from_lines([])`（即 `OcrResult("", 0.0, lines=())`）
  - 遍历 `result.boxes`（四角点）算包围盒 → `BoundingBox`，与 `result.txts`/`result.scores`
    zip 成 `OcrLine` 列表，按 `(y, x)` 排序
  - `OcrResult.from_lines(lines)`
  - 异常兜底 → `OcrResult.from_lines([])`，不崩 IPC server

### 6.3 `src/sublift/ocr/__init__.py`

导出 `PaddleOcrEngine`, `is_paddle_available`，加入 `__all__`。

### 6.4 `src/sublift/cli.py`

- `--engine` choices 加 `"paddle"`
- `_build_ocr_engine` 加 `paddle` 分支（不可用提示 `uv sync --extra paddle`）
- 返回类型注解加 `PaddleOcrEngine`

### 6.5 `src/sublift/ipc/server.py`

- `--engine` choices 加 `"paddle"`
- `main()` 加 factory 分支：`from sublift.ocr.paddle import PaddleOcrEngine` →
  `BridgeHandler(ocr_engine_factory=PaddleOcrEngine).handle`

### 6.6 GUI（Swift）

- `apps/macos/Sources/SubLiftMac/Core/Messages.swift`：`OcrEngineName` 加 `case paddle`；
  更新注释（原「paddle 留待 Phase 3」改为已实现）。
- `apps/macos/Sources/SubLiftMac/UI/SettingsView.swift` +
  `App/SubLiftMacApp.swift`：两处 Picker 加
  `Text("PaddleOCR (跨平台)").tag(OcrEngineName.paddle)`。
- `PipelineClient.start(engine:)` 已透传任意 String 给 `--engine`，**无需改**。

### 6.7 测试 `tests/test_ocr.py`

加 `TestPaddleOcrEngine`（参照 `TestVisionOcrEngine*`）：

- `@pytest.mark.skipif(not is_paddle_available(), reason="rapidocr 未安装")`
- 单测：`isinstance(engine, OcrEngine)`
- 降级测试：monkeypatch `_PADDLE_AVAILABLE=False` → 抛 `RuntimeError`（match "PaddleOCR 不可用"）
- 集成测试 `@pytest.mark.integration` + skip：识别 "Hello"、识别 "你好世界"、空白图返回空
  （复用现有 `_make_text_image`）

### 6.8 文档

- `docs/design/ocr.md`：模块职责表加 `paddle.py` 行；加 PaddleOcrEngine 章节（实现 +
  模型缓存策略 + 三规格 + 不接归因的契约说明）；更新「跨平台演进指引」把 paddle 从示例变实例。
- `README.md`：安装说明加 `uv sync --extra paddle`（首次运行需联网下模型到
  `~/.cache/sublift/rapidocr-models`）；CLI `--engine` 参数说明加 paddle；GUI 引擎选择提 PaddleOCR。
- `docs/REQUIREMENTS.md`：F14 状态 ❌ → ✅。
- `docs/DECISIONS.md`：增 ADR——PaddleOCR 引擎选型（rapidocr v6 + onnxruntime）+
  模型缓存策略（`~/.cache/sublift/rapidocr-models`）+ 编号规则变更（Phase 5 起 `feat-05<阶段><序号>`）。

## 7. 关键技术约束

| 约束 | 说明 | 缓解 |
|---|---|---|
| 首次下载需联网 | rapidocr 首跑从 modelscope.cn 下几十 MB 模型 | 文档与 CLI 输出提示；离线可用 `python -m rapidocr download_models` 预下载 |
| 模型落点 | rapidocr 默认写 site-packages，venv 重建丢失 | 覆盖 `Global.model_root_dir` 到 `~/.cache/sublift/rapidocr-models` |
| 不接归因 | Phase 4.2 归因为 Vision 五阶段设计 | 契约允许 Paddle 不实现 observer；paddle.py 不接 `timing_callback` |
| box 格式转换 | rapidocr boxes 是四角点 (N,4,2)，OcrLine.box 是矩形 | 取 axis-aligned 包围盒转 BoundingBox，clamp 到图像范围 |
| rapidocr API 变动 | 3.x 仍是活跃主线 | 仅影响 paddle.py 内部；Protocol 与核心层零改动 |
| 中文识别精度 | PP-OCRv6 small 默认 | 实测漏字多可升 medium（`model_type` 参数） |

## 8. 阶段验收门

> 验收门须可二值判定，证据记入 `docs/phases/phase5.json` 对应 feat 的 `evidence`。
> 硬质量对照（Vision vs PaddleOCR 的 F1/CER 等）**不纳入 Phase 5**，留待 Phase 5.1 benchmark。

### 8.1 feat-05001（引擎实现 + 依赖）

- [ ] `uv sync --extra paddle` 成功（退出 0），`python -c "import rapidocr"` 退出 0
- [ ] `PaddleOcrEngine()` 构造后，`ls ~/.cache/sublift/rapidocr-models` 含 det / cls / rec
      三个 `.onnx` + rec 字典文件；且 site-packages 内 `rapidocr/models/` **不存在**
      （证明 `model_root_dir` 覆盖生效，未走默认落点）
- [ ] `isinstance(PaddleOcrEngine(), OcrEngine)` 为 True（单测断言）
- [ ] 不可用时抛 `RuntimeError`（单测 monkeypatch `_PADDLE_AVAILABLE=False`，match "PaddleOCR 不可用"）
- [ ] `uv run ruff check .` 退出 0；`uv run mypy src tests` no issues

### 8.2 feat-05002（CLI + IPC server）

- [ ] `uv run sublift extract <视频> --engine paddle -o out.srt` 退出 0；产出 SRT sanity 通过：
      条数 > 0、非空文本条目 > 0、起止时间码单调递增
- [ ] 未装 paddle extra 时 `--engine paddle` 退出非 0，stderr 含 `uv sync --extra paddle`
- [ ] IPC server `--engine paddle` 启动不报错；有跨进程测试覆盖（start_job engine=paddle
      -> progress(ready) 往返，skipif 无 rapidocr）

### 8.3 feat-05003（GUI）

- [ ] `swift build` 退出 0
- [ ] Swift 测试断言 `OcrEngineName.allCases.contains(.paddle)`（或等价 roundtrip）通过
- [ ] GUI 手动验收清单：SettingsView 与 SubLiftMacApp 两处 Picker 均显示 PaddleOCR 选项，
      选中后提取产出 entries 条数 > 0（手动，记入 evidence）

### 8.4 feat-05004（测试 + 文档）

- [ ] `uv run pytest tests/test_ocr.py` 全过（无 rapidocr 时 paddle 测试 skip）
- [ ] `uv run pytest` 全套不回归（与 main 基线 passed/skipped 数一致或更多）
- [ ] `uv run ruff check .` 退出 0；`uv run mypy src tests` no issues
- [ ] `swift test` 全绿
- [ ] `./init.sh` 退出 0
- [ ] 文档逐项勾选：
  - [ ] `docs/design/ocr.md` 含 PaddleOcrEngine 章节
  - [ ] `README.md` 含 `uv sync --extra paddle` 与 `--engine paddle` 说明（含首次下载提示）
  - [ ] `docs/REQUIREMENTS.md` F14 状态标 ✅
  - [ ] `docs/DECISIONS.md` 含新 ADR（引擎选型 + 模型缓存 + 编号规则变更）

## 9. 风险与权衡

| 风险 | 影响 | 缓解 | 状态 |
|---|---|---|---|
| rapidocr 3.x 与 1.x API 不同 | 用错包导致 result 取值错 | 已从源码核实 3.x `RapidOCROutput` 结构；明确选 3.x | 已确认 |
| 首次下载失败（网络/防火墙） | 用户首跑卡住 | 文档提示 + 离线预下载路径 | 文档覆盖 |
| PaddleOCR 中文精度不及 Vision | 字幕质量差异 | `model_type` 可升 medium；后续可开对照 benchmark | Phase 5 不评估，留待 5.1+ |
| 归因系统对 paddle 不可见 | 性能归因报告只覆盖 Vision | Phase 4.2 契约已允许；paddle 性能不在 Phase 5 验收门 | 已声明排除 |
| GUI 用户切到 paddle 但未装依赖 | IPC server 启动失败 | server 端 factory 构造抛 RuntimeError → bridge 回 done(ok=False) | 现有降级路径覆盖 |

## 10. 完成后的决策

Phase 5.0 完成后，以下后续工作候选（不在本 Phase 范围）：

- **Phase 5.1 多引擎对照 benchmark**：复用 Phase 4.2 canonical 负载，Vision vs PaddleOCR
  质量/速度对比报告，为引擎路由提供数据基线。
- **Phase 5.2 引擎自动路由**：按字幕语言/质量自动选引擎。
- **Phase 6 跨平台**：PaddleOCR 是 Windows/Linux 的主引擎候选，GUI 壳跨平台化。
