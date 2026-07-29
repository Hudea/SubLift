# Phase 6.8 — Paddle Native Quality & Performance Hardening

> 子阶段编码：`S = 8` → feature 前缀 `feat-068xx`
> 任务跟踪：`docs/phases/phase6.json`
> 前置：Phase 6.7 已交付可运行的 C++ Paddle Native MVP；质量与性能审计表明它尚不具备产品默认条件
> Python Oracle：`src/sublift/ocr/paddle.py` + `rapidocr==3.9.2` + `onnxruntime==1.28.0` + PP-OCRv6 small
> 计划中的安全策略（`feat-06801` 实施后）：Paddle 默认 Python；C++ Paddle 仅显式 experimental
> 当前 6.7 代码状态：C++ Paddle 可用时仍会自动选择；在 06801 完成前应显式 `--runtime python`

## 1. 为什么需要 6.8

6.7 证明了以下能力：

- `sublift_paddle` 能用 ONNX Runtime 加载 PP-OCRv6 Det/Rec 模型；
- C++ Worker、CLI、GUI runtime policy 能绑定 `engine=paddle`；
- 模型缓存、availability、错误上抛与回滚路径存在；
- Release Worker 可对真实短片导出非空 SRT。

但 6.7 的完成证据只覆盖 Native MVP，并未证明 C++ Candidate 与 Python RapidOCR
在真实推理、字幕质量和性能上达到可默认切换的水位。2026-07-29 审计发现：

1. Det 后处理是阈值 + 连通域 AABB，不是 RapidOCR 的完整 DB/dilation/contour/
   score/unclip；
2. C++ 使用最近邻缩放、AABB crop、逐框 Rec，无完整 Cls/透视 crop/Rec padding parity；
3. Det 无框时执行整图 Rec，与 Python Oracle 的空结果语义不同；
4. Paddle golden 只验证手工构造的行排序/AABB，没有运行 Det/Cls/Rec；
5. cutover GT L3 固定为 Vision，不能证明 C++ Paddle 质量；
6. 同一 2 分钟样片：Python Paddle 54.0s / 44 条，C++ Paddle 147.8s / 43 条；
7. 同一 30 秒样片：非 OCR mock 路径 Python 1.7s、C++ 1.5s，性能差距主要在
   Paddle adapter；C++ ORT 被显式锁成单线程，Python 默认线程在固定 ROI 上约快
   1.95 倍。

因此 6.8 的主题不是继续加功能，而是把“能跑”升级为“可量化、可回归、可默认”。

## 2. 目标与非目标

### 2.1 目标

1. Paddle 默认路由在 hardening 期间 fail-safe 到 Python；C++ Paddle 保留显式实验入口。
2. 建立 Det / Cls / Rec 分阶段可观测性与冻结 Oracle fixture。
3. 对齐 RapidOCR 3.9.2 的 PP-OCRv6 预处理、后处理、crop、解码和空结果语义。
4. 建立 runtime 明确的 Paddle 专项质量门，不借用 Vision 水位或纯几何 golden。
5. 在质量门冻结后优化 ORT 线程、Rec batch、OpenCV 热点和缓冲复用。
6. 质量、性能、稳定性、回滚全部通过后，才重新把 Paddle 默认切到 C++。

### 2.2 非目标

| 不做 | 归属 / 原因 |
|---|---|
| 删除 Python Oracle / benchmark | 6.9+；6.8 仍需双轨对照与回滚 |
| `.app` 公证、随包模型/ORT/ffmpeg、universal2 | 6.9+ 分发 |
| 改 signature/changepoint/timeline/line_select 刷 Paddle 指标 | 禁止；6.8 只修 OCR adapter |
| 用 subprocess 或嵌 CPython 调 RapidOCR 冒充 Native | 不解决产品依赖 |
| 并行多个 Pipeline/OCR job | 不改变串行 Pipeline 所有权 |
| 同时调质量算法与性能参数 | 禁止；先冻结质量，再做性能 |
| 用 Vision GT 数字代表 Paddle | 禁止；必须 `engine=paddle` 且写明 runtime |

## 3. 路线与依赖

| 顺序 | Feature | 名称 | 一句话验收 |
|---:|---|---|---|
| 1 | `feat-06801` | 安全路由与 Experimental 标识 | Paddle 默认 Python；显式 C++ 可用且不静默换引擎 |
| 2 | `feat-06802` | 分阶段观测与冻结 Oracle fixture | Det/Cls/Rec 中间量可同输入对照，模型/配置/环境可复现 |
| 3 | `feat-06803` | Det 预处理 + 完整 DB/unclip parity | 固定概率图与真实 fixture 的 box/score 达到冻结门 |
| 4 | `feat-06804` | Quad crop + Cls + Rec parity | 固定 crop 上分类、文本、置信度与 Python Oracle 对齐 |
| 5 | `feat-06805` | 多源 Paddle E2E 质量门 | runtime 分栏的 frame/clip/GT 指标可阻断回归 |
| 6 | `feat-06806` | Paddle Native 性能加固 | 质量不退化且 canonical wall 达产品门 |
| 7 | `feat-06807` | Paddle C++ 产品 cutover | 默认切换、回滚演练、文档与发布报告全部完成 |

```text
feat-06706 (6.7 MVP)
  └─ feat-06801 safe routing
      └─ feat-06802 stage harness
          └─ feat-06803 Det parity
              └─ feat-06804 Cls/Rec parity
                  └─ feat-06805 E2E quality gate
                      └─ feat-06806 performance
                          └─ feat-06807 cutover
```

每次只实施一个 feature；后一个 feature 不得用“将在后续补门”绕过前一个验收。

## 4. 全局不变量

1. `sublift_core` 不依赖 ORT、RapidOCR、Python 或 Paddle。
2. Paddle 依赖只进入 `sublift_paddle` 及显式链接它的 Worker/测试 target。
3. Python 与 C++ 使用相同 PP-OCRv6 tier、模型文件 SHA256、字典与关键参数。
4. 无文字是空 `OcrResult`；模型/推理/映射故障必须失败，不伪装为空字幕。
5. Candidate 不得静默改成 Vision/Mock；不可用时只能显式回到 Python Paddle。
6. 每个性能改动必须重跑冻结质量门。
7. full runtime 报告必须同时记录 `engine=paddle` 与 `runtime=python|cpp`。
8. 默认 `./init.sh` 不下载模型、不跑长视频；full Paddle gate 走独立脚本或
   `SUBLIFT_INIT_PADDLE=1`，报告写 `/tmp` / `debug`。

## 5. Feature 详述

### feat-06801 — 安全路由与 Experimental 标识

**目的：** 在 hardening 未完成前消除用户默认吃到 Candidate 质量/性能债的风险。

**实现范围：**

- Python `resolve_runtime` 与 Swift `RuntimePolicy`：
  - 未显式请求 C++ 时，`engine=paddle` → Python；
  - `--runtime cpp` / GUI developer override / 明确实验 env 才允许 C++；
  - C++ 不可用时显式 `paddle_override → python`；
  - 永不改写成 Vision/Mock。
- CLI/GUI/日志显示 `paddle-native experimental` 与最终 runtime。
- README、ARCHITECTURE、engine matrix 与实现保持一致。
- 6.7 状态描述改为“Native MVP done”，不得写成“产品质量 parity 完成”。

**验收：**

- Python/Swift 表驱动矩阵覆盖 explicit flag、env、默认、不可用四类情况；
- 默认 `--engine paddle` 启动 Python Worker；
- 显式 `--engine paddle --runtime cpp` 在可用环境启动 C++ Worker；
- capability/error 文案不隐瞒 runtime；
- ruff/mypy/pytest/Swift tests/C++ 相关测试全绿。

**回滚点：** 只改变 policy 与文档，不改 OCR 算法；可独立回退。

---

### feat-06802 — 分阶段观测与冻结 Oracle Fixture

**目的：** 在改算法前建立可归因的 A/B，不再只比较最终 SRT。

**阶段树：**

```text
recognize
  ├─ input/global_preprocess
  ├─ det/preprocess
  ├─ det/infer
  ├─ det/postprocess
  ├─ crop/perspective
  ├─ cls/preprocess + infer + rotate
  ├─ rec/preprocess
  ├─ rec/infer
  ├─ rec/decode
  └─ output/filter_sort
```

**冻结内容：**

- RapidOCR、onnxruntime、OpenCV 版本；
- PP-OCRv6 model tier、模型 SHA256、字典/metadata SHA256；
- 所有关键参数：limit side/type、mean/std、thresh、box_thresh、unclip_ratio、
  dilation、score_mode、text_score、rec shape/batch、Cls threshold；
- 许可干净的小型 fixture：
  - empty；
  - 单行/双行；
  - CJK/Latin/混排；
  - 彩色描边、低对比、轻微模糊；
  - 倾斜/旋转；
  - 粘连/相邻框。

**dump schema：**

- 输入尺寸/色序/归一化 tensor 摘要；
- Det probability map shape/hash/stat；
- 四角框、score、排序；
- perspective crop shape/hash；
- Cls label/score；
- Rec tensor shape、logits 摘要、CTC tokens/text/confidence；
- 最终 `OcrLine`；
- 各阶段 wall/count，不把子阶段相加为多份总耗时。

**验收：**

- Python/C++ dump 可由同一 manifest 驱动；
- schema version、Oracle commit、依赖版本、模型 hash 均入报告；
- fixture 不依赖网络；
- off 模式无逐帧大对象落盘；
- stage count 与 recognize call count 可对账；
- 本 feature 不改变现有 OCR 输出。

---

### feat-06803 — Det 预处理与完整 DB/unclip Parity

**目的：** 替换简化连通域 AABB，先收敛框，再动 Rec。

**实现范围：**

- 与 RapidOCR 对齐的全局 resize/vertical padding 映射；
- `cv::resize(..., INTER_LINEAR)`；
- RGB/BGR、NCHW、mean/std；
- threshold；
- 2×2 dilation；
- `findContours(RETR_LIST, CHAIN_APPROX_SIMPLE)`；
- `minAreaRect` / ordered quad；
- fast polygon mask score；
- max candidates/min-size 过滤；
- Clipper polygon offset / `unclip_ratio=1.6`；
- 映射回原图、clip、过滤、排序；
- 内部保留 quad；只在 SubLift `OcrLine` 边界转 AABB；
- Det 无框立即返回空结果，禁止整图 Rec。

**复用策略：**

- 优先参考 RapidOCR 3.9.2 Python 与 RapidAI `RapidOcrOnnx` 的 Apache-2.0
  C++ 实现；
- 不整仓引入旧版 RapidOcrOnnx；
- 只移植/改写必要算法，并补 LICENSE/NOTICE 与 PP-OCRv6 fixture；
- OpenCV/Clipper 只进入 `sublift_paddle`，不污染 `sublift_core`。

**冻结门：**

| 层 | 门 |
|---|---|
| Det input tensor | shape exact；数值 `max_abs <= 1e-5` |
| probability map | shape exact；同 ORT provider 下 `max_abs <= 1e-5` |
| 固定 probability-map postprocess | box count exact；坐标误差 ≤1px；score abs ≤1e-4 |
| fixture box matching | precision/recall = 1.0 @ IoU 0.95，或书面列出平台 rounding 例外 |
| empty fixture | 0 box、0 Rec call、空 `OcrResult` |

**验收命令：**

- 无模型的纯 postprocess Catch2 fixture；
- 有模型环境的 Python/C++ Det dump 对照；
- 全量 C++ tests + 标准 Python 静态/单测门。

---

### feat-06804 — Quad Crop、Cls 与 Rec Parity

**目的：** 在框已冻结后，对齐“框如何变成文字”。

**实现范围：**

- 四角框 perspective transform / warp；
- 高窄 crop 的旋转规则；
- RapidOCR vertical padding 与原坐标回映；
- Cls 模型加载、48×192 预处理、180° rotate、阈值；
- Rec `3×48×320` 基准、动态 max width ratio、右侧零 padding；
- width 排序和 batch（先实现语义，性能调优留 06806）；
- 模型 metadata 字符表优先，外部字典只作经过 hash 校验的 fallback；
- CTC blank/duplicate/space、置信度 rounding；
- `text_score=0.5` 过滤；
- 最终空行过滤、AABB clamp、稳定 `(y,x)` 排序。

**冻结门：**

| 层 | 门 |
|---|---|
| perspective crop | 尺寸误差 ≤1px；像素 mean abs ≤1.0、P99 ≤3（0–255） |
| Cls | label exact 100%；score abs ≤1e-4 |
| Rec input tensor | shape exact；有效区 `max_abs <= 1e-5`；padding exact zero |
| Rec fixed crops | text exact 100%；confidence abs ≤1e-4 |
| batch parity | batch=1 与 batch=N 的逐项输出 exact |
| final lines | text exact；AABB 坐标误差 ≤1px；顺序 exact |

**验收：**

- empty/upright/180°/two-line/mixed fixture 全绿；
- 无 Cls 模型时 availability 诚实失败，不静默跳过；
- 不改变 Pipeline/line_select。

---

### feat-06805 — 多源 Paddle E2E 质量门

**目的：** 把“实现 parity”提升为“字幕产品质量不退化”。

**数据集分层：**

| 层 | 内容 | 默认 init |
|---|---|---|
| Q0 committed fixtures | 小图、概率图、crop、中间量 | 可进入，不跑模型或有明确 skip |
| Q1 fixed short clips | 至少 CJK、Latin、混排；视频可外置但 manifest+SHA256 固定 | 不进入 |
| Q2 GT clips | ≥3 个来源、总时长 ≥10min、版本化 SRT GT | 发布/显式 gate |

Q1/Q2 必须覆盖无字幕间隔、单/双行、描边、不同位置、低对比、运动背景；不得只用
Zootopia 单一样式。

**报告矩阵：**

```text
engine=paddle, runtime=python  (Oracle)
engine=paddle, runtime=cpp     (Candidate)
                         ↓
同一 benchmark diagnostics：timing / CER / usable / noise / empty / box metrics
```

**Candidate 相对 Oracle 硬门：**

- timing F1、precision：下降不超过 1.0 个百分点；
- CER macro/micro：绝对增加不超过 1.0 个百分点；
- usable subtitle recall：下降不超过 2.0 个百分点；
- noise/empty：每 clip 不高于 Oracle +1，全集不得系统性增加；
- matched line box recall ≥0.98 @ IoU 0.5，mean IoU ≥0.90；
- job failure、engine mismatch、model missing 语义 exact。

**绝对 GT 门：**

- 先运行 Python Oracle 固定 Paddle 水位并归档；
- Candidate 同时满足相对门和已冻结的绝对门；
- 阈值只能由新增 GT/指标定义 ADR 调整，禁止为 Candidate 临时放松。

**产物：**

- `scripts/parity/check_paddle_gate.py`（或同职责入口）；
- JSON + Markdown 报告；
- runtime/engine/model hash/commit/config fingerprint；
- 报告默认写 `/tmp`，发布时显式归档。

---

### feat-06806 — Paddle Native 性能加固

**目的：** 在质量冻结后消除当前 2.5–2.8× wall 回退。

**优化顺序：**

1. ORT thread sweep：1 / 2 / 4 / default；Det/Cls/Rec 可分别配置但默认保持简单；
2. Rec batch：1 / 2 / 4 / 6，保持 batch parity；
3. 空 Det 不进 Rec；
4. OpenCV SIMD resize/color/normalize 与完整 DB 热点；
5. session、input/output name、MemoryInfo 长生命周期复用；
6. crop/BGR/float tensor scratch buffer 复用；
7. 减少 vector reallocation 与逐像素 `std::queue`；
8. 必要时再评估 ORT execution provider；不在本 feature 引入 GPU 产品依赖。

**测量纪律：**

- 同机、同电源、同模型、同 clip、Release；
- warm-up 后 Python/C++ 交错至少 3 轮，比较 median；
- 分离 startup、extractor/pipeline、Det、Cls、Rec、postprocess；
- 同时记录 user/sys/wall、峰值 RSS、OCR call/box/Rec batch 数；
- 每个优化点单独 A/B，质量 hash/gate 同时运行。

**性能门：**

| 指标 | 硬门 | 目标 |
|---|---:|---:|
| canonical 2min wall median | C++ ≤ Python ×1.20 | C++ ≤ Python ×1.10 |
| 非 OCR mock control | C++ ≤ Python ×1.20 + 0.2s | 不退化 |
| cancel | ≤1s | 保持 |
| restart | ≤5s | 保持 |
| RSS | C++ ≤ Python ×1.25 | 记录绝对值 |
| 质量 | 06805 全部硬门 | 不允许任何门因性能优化放宽 |

若硬门未过，06806 不得 `done`，06807 不得开始。

---

### feat-06807 — Paddle C++ 产品 Cutover

**目的：** 只在 Candidate 已证明正确且性能可接受后恢复 C++ 默认。

**切换行为：**

- `engine=paddle` + C++ Paddle available → C++ Worker；
- unavailable → 显式 Python Paddle override；
- `SUBLIFT_RUNTIME=python` / CLI / GUI override 始终可一键回滚；
- 禁止静默 Vision/Mock；
- 日志与 GUI 显示最终 runtime、model tier、experimental/stable 状态。

**发布门：**

- 06803/06804 stage parity；
- 06805 Q1/Q2 quality；
- 06806 performance；
- default init；
- Paddle-enabled Release build + ctest；
- Python tests / mypy / ruff；
- Swift build/tests；
- cancel/restart；
- ≥10min Paddle C++ 长流；
- rollback 演练：默认 cpp → 强制 Python → cpp restart，无串扰。

**完成产物：**

- engine matrix、README、ARCHITECTURE、REQUIREMENTS；
- release/cutover Markdown 报告；
- phase6.json evidence；
- feature-list 状态；
- progress 导航；
- Python Paddle 至少保留一个小版本周期。

## 6. 6.8 完成定义

只有同时满足以下条件，Phase 6.8 才可宣告完成：

- [ ] `feat-06801`–`feat-06807` 全部 `done`；
- [ ] C++ Paddle 不再包含简化连通域或无框整图 fallback；
- [ ] Det/Cls/Rec stage fixture parity 全绿；
- [ ] 多源 Paddle GT 相对/绝对质量门全绿；
- [ ] canonical 性能硬门全绿；
- [ ] 默认 C++ Paddle 与 Python 回滚都经过真实 CLI/GUI 验收；
- [ ] 标准 `./init.sh` 全绿且未塞入长视频/模型下载；
- [ ] 验证证据记录于 `docs/phases/phase6.json`；
- [ ] 6.9+ 才开始删除 Python 产品依赖或处理分发打包。

## 7. 风险与止损

| 风险 | 止损 |
|---|---|
| RapidOcrOnnx 代码较旧，和 PP-OCRv6 不完全兼容 | 只借鉴算法；以 RapidOCR 3.9.2 fixture 为 Oracle |
| 完整 DB 引入 OpenCV/Clipper 许可与构建复杂度 | target 隔离、NOTICE、OFF 构建不回归 |
| 平台浮点/rounding 导致非 exact | 中间量 exact 优先；坐标 ≤1px / 数值 epsilon 明确写死 |
| 为追性能改变质量 | 06805 先冻结；06806 每步重跑 |
| GT 多样性不足 | 06805 不允许只用现有 Zootopia |
| C++ 仍慢于 Python | 默认继续 Python；不为了路线图强行 cutover |
| 模型/ORT 无法随包 | 6.9+；不阻塞 6.8 开发态质量收口 |

## 8. 6.9+ 边界

6.8 完成后，6.9+ 才评估：

- 去 Python 产品 runtime；
- `.app` 内置 ORT / PP-OCRv6 模型；
- ffmpeg 随包；
- universal2、codesign、notarization；
- Windows/Linux 产品分发；
- Python Oracle 是否只保留为开发依赖。

这些工作不得倒逼 6.8 降低 Paddle 质量、性能或回滚门。
