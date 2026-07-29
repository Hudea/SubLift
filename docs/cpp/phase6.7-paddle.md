# Phase 6.7 — PaddleOCR C++ Adapter（ONNX / PP-OCRv6）

> 子阶段编码：`S = 7` → feature 前缀 `feat-067xx`  
> 任务跟踪：`docs/phases/phase6.json`  
> 总览：[phase6-overview.md](phase6-overview.md)  
> 契约：[architecture.md](architecture.md) · [parity-contract.md](parity-contract.md) · [engine-matrix-and-cutover.md](engine-matrix-and-cutover.md) · [worker-ipc-contract.md](worker-ipc-contract.md)  
> **门槛：** 6.6 cutover 已完成（feat-06601–06605）；vision/mock 产品默认 C++；paddle **当前** 仍仅 Python  
> **Oracle：** `src/sublift/ocr/paddle.py`（`PaddleOcrEngine` + rapidocr PP-OCRv6 + onnxruntime）

## 1. 目标

在 **不推翻 `IOcrEngine` 边界、不改打轴/行选算法** 的前提下，为 **`engine=paddle`** 提供 **C++ 原生 OCR adapter**，使 C++ `sublift_worker` 能声明并运行 paddle，逐步去掉「paddle 必须 Python worker」的硬路由。

```text
ImageView (RGB24 优先，与 extractor / Vision 一致)
    → 适配为 RapidOCR/ONNX 期望的输入（见 §5 颜色契约）
    → PP-OCRv6 Det (+ 可选 Cls) + Rec（ONNX Runtime）
    → 四角点 box → axis-aligned BoundingBox + clamp
    → sort (y, x)
    → OcrResult::from_lines
```

| 交付 | 说明 |
|---|---|
| **`sublift_paddle` target** | 实现 `PaddleOcrEngine : IOcrEngine`；**不**链入 `sublift_core` |
| 模型与运行时 | 与 Python Oracle **同系**：PP-OCRv6 + **ONNX Runtime**；默认 `small`；缓存目录对齐 |
| CMake 门控 | `SUBLIFT_ENABLE_PADDLE`（默认 **OFF**）；缺 ONNX/模型时 fail-closed |
| Worker 矩阵 | `engine=paddle` 可 bind C++ worker；capability 诚实 |
| Runtime 策略 | 有 C++ paddle 时允许 `runtime=cpp`；否则保留 `paddle_override → Python` |
| Parity | box/颜色 L0；合成图/固定夹具 L2–L3；live 文本 L4；**不**要求与 Vision 同字 |

**本子阶段结束时（理想状态）：**

- macOS / Linux（有 ONNX）可构建并运行 **C++ paddle**；
- 产品默认：`paddle` → **优先 C++ worker**（若已构建且可用），否则 **显式** 回退 Python（不静默改引擎）；
- Python `PaddleOcrEngine` **保留**为 Oracle / 回滚 / 无 ONNX 环境。

**本子阶段不结束：** 删除 Python 树、`.app` 内嵌模型与公证、去 `uv` 的完整无 Python 分发（见 §2 不做 / 6.8+）。

## 2. 做 / 不做

### 做

1. 新增 CMake target **`sublift_paddle`**（或等价命名），仅依赖 `sublift_core` + **ONNX Runtime**（系统/包管理安装，**不** FetchContent 整棵 ORT）。
2. 实现 `PaddleOcrEngine::recognize(ImageView)`，对齐 Python 故障语义：
   - **无检测文本** → 空 `OcrResult`；
   - **模型加载 / 推理 / box 映射异常** → 向上抛，Worker 转 `done(ok=false)`，**禁止**伪装成空字幕。
3. **模型规格**：`tiny` / `small`（默认）/ `medium` 与 Python `_VALID_MODEL_TYPES` 一致；缓存根默认 `~/.cache/sublift/rapidocr-models`（可配置 env / 构造参数）。
4. **四角点 → AABB**：floor/ceil 与 clamp、缺失 box 时整图 fallback、排序 `(y,x)` 与 Python 一致（L0 表驱动）。
5. **颜色契约**：文档 + 单测锁定「RGB24 入、推理前是否转 BGR」与 Oracle 一致（Python：PIL → RapidOCR 内部 RGB→BGR；**禁止**把 RGB ndarray 当 BGR 喂进网络）。
6. **Worker：** `EngineFactory` 支持 `bound_engine=paddle`；handshake 仅声明本进程引擎；去掉「paddle 永不支持 C++」的硬错误（改为「本构建未启用 / 模型不可用」）。
7. **Runtime 解析：** 扩展 `resolve_runtime` / Swift `RuntimePolicy`：
   - `engine=paddle` + C++ paddle **可用** → 允许 `cpp`（默认可切到 cpp，见 feat-06706）；
   - **不可用** → 保持 `paddle_override → python`（诚实提示，不静默 vision/mock）。
8. Parity harness：`dump_paddle.py` + C++ 对照；init/cutover 可选门（见 §6）。
9. 文档：引擎矩阵、architecture target 图、README、CLI/GUI 提示文案更新。

### 不做

| 项 | 归属 / 理由 |
|---|---|
| 改 signature / changepoint / line_select / conf 阈值「刷」GT | 禁止；parity 以 Oracle 为准 |
| 嵌入完整 PaddlePaddle 训练框架 | 过重；Oracle 已是 ONNX 推理 |
| 用 subprocess 调 Python rapidocr 冒充 native | 不解决产品依赖 |
| 删除 `src/sublift` / 去掉 Python oracle | **6.8+** 且全引擎有 native 或产品放弃 |
| macOS 公证、随包 ffmpeg、universal2 收尾 | **6.8+ / 分发** |
| Vision 路径改动 | 非本子阶段 |
| 并行 `recognize` / 多线程 ORT session 共享无锁 | 禁止；与串行 Pipeline 契约一致 |
| 要求 paddle 文本与 Vision **逐字**一致 | 禁止；两引擎 L4 独立 |
| 默认把 Linux 产品引擎从「无」强行改成 paddle 并切默认 | 可登记；**默认策略变更单独立项验收** |

## 3. 技术选型（冻结建议）

| 方案 | 结论 |
|---|---|
| **A. ONNX Runtime + PP-OCRv6 模型（与 rapidocr 同系）** | **采用（推荐）** |
| B. Paddle Inference C++ 全栈 | 否：体积/构建复杂，且与当前 Python extra 不一致 |
| C. 链 Python C-API 调 rapidocr | 否：仍依赖嵌入解释器 |
| D. 自研 det/rec | 否：超出 6.7 范围 |

### 3.1 与 Python Oracle 的对应

| Python | C++ 6.7 |
|---|---|
| `rapidocr.RapidOCR` | 自研或薄封装的 Det/Cls/Rec 流水线（**行为对齐**，不要求 ABI 兼容 rapidocr） |
| `onnxruntime` | **ONNX Runtime C++ API** |
| `Global.model_root_dir` ≈ `~/.cache/sublift/rapidocr-models` | 同默认路径；可用 `SUBLIFT_PADDLE_MODEL_DIR` 覆盖 |
| `Det.model_type` / `Rec.model_type` = tiny/small/medium | 同枚举；默认 **small** |
| 输入 PIL → 内部 BGR | `ImageView` RGB24 → 明确转换点（单测锁定） |
| `txts is None` → 空结果 | 同 |
| 推理异常向上抛 | 同 |

### 3.2 模型文件

- **优先复用** rapidocr 已下载的 ONNX 文件名/布局（若稳定可文档化映射表）。
- 首次运行允许下载到缓存目录（需网络）；CI 应用 **预置夹具模型** 或 skip 标记，禁止 flaky 外网门。
- 许可：PP-OCR / ORT 许可证写入 `cpp/README` 与第三方 NOTICE（实现阶段补齐）。

### 3.3 平台

| 平台 | 6.7 目标 |
|---|---|
| **macOS arm64** | 必达构建 + 烟测（开发机） |
| **Linux x86_64/arm64** | **至少** `ENABLE_PADDLE=ON` 可配置构建；CI 可选 job |
| Windows | 不在 6.7 必达 |

`sublift_core` / Vision / ffmpeg **不得**因 paddle OFF 而无法构建。

## 4. Target 与依赖图

```text
sublift_core (IOcrEngine, models, pipeline)
       ▲
       │
sublift_paddle ──► ONNX Runtime (system)
       ▲
       │  (optional link)
sublift_worker / sublift_cli
```

| Target | 允许 | 禁止 |
|---|---|---|
| **`sublift_paddle`** | core、ORT、（可选）最小图像工具 | ObjC、Vision、UDS、Swift |
| **`sublift_core`** | 不变 | **不得** `#include` paddle/ORT |
| **`sublift_worker`** | 按 option 链接 paddle | 默认 OFF 时无 ORT 符号 |

CMake 选项（建议）：

```text
SUBLIFT_ENABLE_PADDLE=OFF          # 默认
SUBLIFT_REQUIRE_PADDLE=OFF         # ON 时找不到 ORT 则 configure 失败
SUBLIFT_PADDLE_MODEL_DIR=...       # 可选缓存/夹具路径
```

## 5. 契约细节

### 5.1 `IOcrEngine` 表面

```text
// 与 Vision/Mock 相同
class PaddleOcrEngine final : public IOcrEngine {
 public:
  explicit PaddleOcrEngine(PaddleOcrOptions opts = {});
  OcrResult recognize(const ImageView& image) override;
};
```

构造选项建议：`model_type`、`model_root_dir`；语言列表若 rapidocr 默认可不暴露（与 Python 当前表面一致：无 per-request lang API）。

### 5.2 颜色与像素

| 规则 | 约定 |
|---|---|
| 产品输入 | Pipeline crop / ROI = **RGB24**（与 6.3/6.4 一致） |
| 网络输入 | PP-OCR ONNX 通常期望 **BGR**；在 `sublift_paddle` **内部**转换，不污染 core |
| 禁止 | 把 RGB buffer 不经转换按 BGR 解释（Python 已知踩坑） |
| Gray | 文档化：升为 RGB 或拒绝；须与 dump 一致 |

### 5.3 Box 与行

与 `paddle.py`：

1. `boxes` 四角点 → `x_min=floor(min x)` … `y_max=ceil(max y)`  
2. clamp 到 `[0,w]×[0,h]`  
3. 空 `text.strip()` 丢弃  
4. `boxes is None` 且有 txts → 整图 fallback box  
5. `sort (y, x)` → `OcrResult::from_lines`

### 5.4 故障语义

| 情况 | 行为 |
|---|---|
| 无文本检测 | 空 `OcrResult` |
| ORT 未链接 / 模型文件缺失 | 构造或 `is_paddle_available()==false`；Worker 拒绝 bind 或 start_job 明确错误 |
| 推理中异常 | 抛 → job fail |
| `engine=paddle` 但 worker 以 vision 启动 | 既有 engine mismatch |

### 5.5 Capability / Runtime 矩阵（6.7 目标）

| engine | C++ paddle 可用 | C++ paddle 不可用 |
|---|---|---|
| vision / mock | C++（6.6 已成立） | — |
| **paddle** | **C++ worker（默认倾向）** | **Python worker（override）** |

禁止：paddle 请求落到 vision/mock。

解析优先级保持：

```text
显式 --runtime / GUI 覆盖
  → SUBLIFT_RUNTIME
  → 产品默认
  → paddle 可用性门（不可用 cpp 时 override 到 python）
```

## 6. 任务一览

| ID | 名称 | 验收一句话 |
|---|---|---|
| **feat-06701** | 纯契约：颜色/四角点 AABB/clamp/排序 | 无 ORT 的表驱动 L0；与 `paddle.py` 数值对齐 |
| **feat-06702** | CMake + ONNX Runtime 发现 + target 壳 | `ENABLE_PADDLE=ON` 可配置；OFF 时 core/worker/vision 无回归 |
| **feat-06703** | `PaddleOcrEngine::recognize` 主路径 | Det/Rec（+Cls 若 Oracle 默认开）跑通；空图/故障语义 |
| **feat-06704** | 模型规格 + 缓存路径 + availability | tiny/small/medium；默认缓存；`is_paddle_available` 语义 |
| **feat-06705** | Worker / CLI / Runtime 接线 | `--engine paddle` 可 bind C++；policy 更新；GUI 不静默改引擎 |
| **feat-06706** | Parity harness + paddle cutover 门 | dump/golden + 可选 GT/合成水位；默认路由策略文档化并测 |

依赖：

```text
feat-06605 (cutover done) + feat-06201 (IOcrEngine)
    └── feat-06701 pure geometry/color contracts
            └── feat-06702 cmake + ORT skeleton
                    └── feat-06703 recognize implementation
                            └── feat-06704 models + availability
                                    └── feat-06705 worker/runtime integration
                                            └── feat-06706 parity + product default for paddle
```

**推荐一次一刀：** 06701 → 06702 → 06703 → 06704 → 06705 → 06706。  
**不要**在 06703 未稳定前改产品默认路由。

## 7. 任务详述

### feat-06701 — 纯契约（无 ORT）

| 必须 | 说明 |
|---|---|
| API | 例如 `quad_to_aabb`、`clamp_box`、`rgb_to_bgr_nchw_or_hwc` 等纯函数 |
| Oracle | `src/sublift/ocr/paddle.py` 中 box/clamp/sort 行为 |
| 单测 | Catch2 表驱动；**不**链接 ORT |
| 位置 | `sublift_paddle` 内 `.cpp` 或 `paddle_geometry.hpp`，便于 OFF 时仍可测纯函数（或放 test_support） |

### feat-06702 — CMake / ORT 骨架

| 必须 | 说明 |
|---|---|
| `find_package` / 提示安装 | 文档写清 macOS/Linux 安装 ORT 方式 |
| target | `sublift_paddle` 静态库壳 + `is_paddle_available()` stub |
| OFF 路径 | 默认构建与 `./init.sh` **不**要求 ORT |
| worker | OFF 时仍拒绝 paddle（与今日相同错误语义可保留） |

### feat-06703 — recognize 主路径

| 必须 | 说明 |
|---|---|
| 输入 | `ImageView` RGB24（及文档化的其它格式策略） |
| 流水线 | 对齐 rapidocr 默认：Det →（Cls）→ Rec |
| 输出 | `lines` + `from_lines` |
| 测试 | 合成图英文/中文（夹具）；空白图；可选 skip 无模型 |

### feat-06704 — 模型与可用性

| 必须 | 说明 |
|---|---|
| model_type | tiny/small/medium |
| 目录 | 默认 `~/.cache/sublift/rapidocr-models` |
| 下载策略 | 开发可下；CI 用 vendored 小夹具或 skip |
| 错误信息 | 指导用户安装 ORT / 模型 / 回退 Python |

### feat-06705 — Worker / Runtime / GUI·CLI

| 必须 | 说明 |
|---|---|
| `EngineFactory` | `paddle` bind + create |
| capability | 仅声明本进程引擎 |
| `resolve_runtime` | 可用性感知的 paddle 路由 |
| GUI | 选 Paddle 时：C++ 可用则 C++，否则 Python；**禁止**静默 Vision |
| CLI | `--engine paddle` 在 C++ 默认 runtime 下可跑（当 paddle 已构建） |
| 日志 | 启动行明确 `engine=paddle` runtime=cpp\|python |

### feat-06706 — Parity 与 cutover 门

| 必须 | 说明 |
|---|---|
| dump | Python oracle dump vs C++ candidate |
| L0 | box/颜色 |
| L2/L3 | 合成或固定短 clip；文本 L4 记录 |
| 门禁 | 可选接入 cutover 脚本「paddle 分支」；vision 门不受影响 |
| 默认 | 文档化：何时 paddle 默认 cpp；回滚 `SUBLIFT_RUNTIME=python` 仍覆盖全引擎 |

## 8. 验收总门（子阶段 done）

同时满足：

1. `SUBLIFT_ENABLE_PADDLE=ON` 构建的 worker 能完成 path mode + `engine=paddle` 导出非空 SRT（有模型环境）。
2. `ENABLE_PADDLE=OFF` 的默认构建 / init 基线不回归。
3. 纯契约 L0 单测全绿；parity 报告归档 `docs/reports/` 或 `debug/`（按仓库惯例）。
4. Runtime 矩阵更新：`engine-matrix-and-cutover.md` 与实现一致；无静默 fallback。
5. 标准验证：`ruff` / `mypy` / `pytest`（Python 侧 policy 单测）+ C++ ctest 相关子集。
6. **不**删除 Python paddle 路径；Oracle 仍可 `uv run --extra paddle`。

## 9. 风险与缓解

| 风险 | 缓解 |
|---|---|
| rapidocr 后处理与自研后处理微差导致 CER 升高 | 先锁预处理/box；文本 L4；必要时对照 ORT 输出 logits 层 |
| ORT 版本/ABI 地狱 | 文档钉版本范围；CI 固定一种安装源 |
| 模型下载 flaky | CI 不依赖外网；夹具模型 |
| 体积膨胀 | paddle option 默认 OFF；分发另议 |
| 与 6.6 paddle_override 行为冲突 | 06705/06706 明确迁移表与单测矩阵 |
| 性能预期 | 目标为 **去 Python 依赖**，不承诺必快于 rapidocr |

## 10. 与 6.6 / 6.8 的边界

| 主题 | 6.6（已完成） | **6.7（本文）** | 6.8+（后置） |
|---|---|---|---|
| vision/mock 默认 C++ | ✅ | 保持 | 保持 |
| paddle 实现 | 仅 Python | **C++ adapter** | 打磨/量化 |
| paddle 默认 runtime | 强制 Python | **可用则 C++** | 可去 Python |
| 删除 Python 产品依赖 | 否 | 否 | 评估 |
| `.app` 模型随包 / 公证 | 否 | 设计预留 | 实施 |

## 11. 文档与跟踪更新清单（设计落地时）

| 文件 | 动作 |
|---|---|
| 本文 `phase6.7-paddle.md` | 设计源头 |
| `phase6-overview.md` / `docs/cpp/README.md` / `NAMING.md` | 登记 6.7 |
| `engine-matrix-and-cutover.md` | 矩阵改为「6.7+ paddle native」 |
| `architecture.md` | target 图实线 `sublift_paddle` |
| `docs/phases/phase6.json` | feat-06701–06706 `not-started` |
| `feature-list.json` | `phase6.paddle-native` 功能块 |
| `docs/DECISIONS.md` | ADR：选型 ONNX + 路由策略 |
| `progress.md` | 当前子阶段指向 6.7 |

## 12. 建议执行顺序（实现阶段）

1. **06701** 纯函数 + 单测（零外部依赖）  
2. **06702** CMake 发现 ORT（本机装通）  
3. **06703–06704** 最小 recognize + small 模型  
4. **06705** 接通 worker，CLI 冒烟  
5. **06706** parity 与默认路由翻转（单独 PR，带门禁）

---

*设计状态：草案已写入跟踪（2026-07-29）。实现前若 ORT 安装源或 rapidocr 模型布局有变，先修订 §3 再动代码。*
