# Phase 6.6 — Cutover（默认路径切换 + 验收门 + 回滚）

> 子阶段编码：`S = 6` → feature 前缀 `feat-066xx`  
> 任务跟踪：`docs/phases/phase6.json`  
> 总览：[phase6-overview.md](phase6-overview.md)  
> 契约权威：**[engine-matrix-and-cutover.md](engine-matrix-and-cutover.md)** · [worker-ipc-contract.md](worker-ipc-contract.md) · [parity-contract.md](parity-contract.md)  
> **门槛：** `feat-06501`–`feat-06505` 全部 `done`（已满足）  
> **Oracle / 对照：** 冻结 Python 产品路径 + 既有 benchmark / GT 水位；C++ 为 Candidate

## 1. 目标

把 **vision / mock** 的**产品默认 runtime** 从 Python Worker 切到 **C++ `sublift_worker`**，同时：

```text
engine ∈ {vision, mock}  →  默认 spawn C++ worker（或原生 CLI 走 core）
engine == paddle         →  仍显式 spawn Python IPC server
SUBLIFT_RUNTIME=python   →  一键回滚整段默认（或按 CLI/GUI 覆盖）
```

| 交付 | 说明 |
|---|---|
| 运行时解析策略 | 统一「默认 / 强制 python / 强制 cpp」规则（env + CLI + GUI） |
| GUI 双 Worker 启动 | `PipelineClient`（或上层）按引擎选可执行文件 |
| CLI 默认路径 | vision/mock 不依赖 `uv` 也能跑（原生 `sublift` 或包装） |
| Cutover 门 | Mock L2、ROI、GT L3、cancel/restart、wall/RSS 记录式门 |
| 回滚 | 文档 + 可验证的 `SUBLIFT_RUNTIME=python`（及 GUI 等价） |
| Release note | 默认切换说明、paddle 仍 Python、已知限制 |

**本子阶段结束时：** macOS 产品主路径（vision）**默认**为 C++ worker；Python 保留为 **oracle / paddle / 回滚**。  
**不做：** 删除 Python 树、原生 Paddle、libav、公证打包（6.7+）。

## 2. 做 / 不做

### 做

1. **冻结并实现**引擎×runtime 矩阵（契约 §1）：默认 cpp 仅 vision/mock；paddle 永不静默改引擎。
2. **统一 runtime 解析**（建议顺序，实现须文档化）：
   - 显式 CLI/GUI 覆盖  
   - `SUBLIFT_RUNTIME=python|cpp`  
   - 产品默认（cutover 后 = `cpp`，但对 paddle 无效）  
3. **GUI：** 启动 C++ worker 二进制（开发：`build/cpp/sublift_worker`；app：`Contents/MacOS` 或 Helpers）；paddle 仍走现 Python 路径。
4. **CLI：** `sublift extract`（或原生 `cpp` CLI）默认对 vision/mock 走 C++；`--runtime` / env 可强制 Python。
5. **Capability UI：** 仅展示当前 worker 声明的 engines；paddle 在 cpp 默认下显示「需 Python runtime」或自动切 Python worker。
6. **Cutover 门自动化或半自动脚本：** 同机对照报告；**任一 GT 硬指标跌破水位 → 阻止合并默认切换**。
7. **Cancel ≤1s / restart ≤5s** 至少 **记录式** 测（契约 §3.2）；硬失败阈值写入本子阶段 evidence。
8. **回滚演练：** 默认 cpp 后设 `SUBLIFT_RUNTIME=python` 恢复 GUI/CLI 行为的测试或清单。
9. 更新 README / progress / release 说明；**保留** Python worker 至少一个小版本周期（契约 §3.4）。

### 不做

| 项 | 归属 |
|---|---|
| 删除 `src/sublift` 产品代码 | **6.9+** 且 6.8 Paddle hardening 已过门，或产品放弃 Paddle |
| 原生 Paddle / ONNX adapter | **6.7** — [phase6.7-paddle.md](phase6.7-paddle.md) |
| 全面重写 SwiftUI | 仅改 launch / runtime 选择 |
| 随包 ffmpeg / 公证 / universal2 收尾 | **6.9+** |
| 改打轴/OCR 阈值「刷」GT | 禁止；以 Oracle 行为为准 |
| Linux 上假装有 Vision | capability 诚实 |

## 3. 任务一览

| ID | 名称 | 验收一句话 |
|---|---|---|
| **feat-06601** | Runtime 解析策略 + 开关面 | env/CLI/文档统一；单测或表驱动锁优先级；**尚不**改 GUI 默认 |
| **feat-06602** | macOS GUI 双 Worker 启动 | vision/mock→C++ bin；paddle→Python；engine mismatch 失败可观测 |
| **feat-06603** | CLI cutover 路径 | 默认 vision/mock 不强制 uv；`--runtime` 可回滚 Python |
| **feat-06604** | Cutover 门：正确性 + 运行时 | Mock L2/ROI + GT L3 报告模板；cancel/restart 测记入 evidence |
| **feat-06605** | 默认翻转 + 回滚演练 + release note | 产品默认 cpp；回滚路径测通；跟踪/文档对齐 6.6 done |

依赖：

```text
feat-06505 (C++ worker opt-in done)
    └── feat-06601 runtime policy (no default flip)
            └── feat-06602 GUI dual launch
            └── feat-06603 CLI dual launch   (可与 06602 并行，合并验收前都完成)
                    └── feat-06604 gates (可用 06602/03 的 spawn)
                            └── feat-06605 default flip + rollback + RN
```

**推荐顺序：** 06601 → 06602 → 06603 → 06604 → 06605。  
（06602/06603 在 06601 后可并行开发，但 **06605 必须最后**。）

---

## 4. 任务详述

### feat-06601 — Runtime 解析策略 + 开关面

| 必须 | 说明 |
|---|---|
| 规格 | 写清优先级：显式 flag > `SUBLIFT_RUNTIME` > 产品默认 |
| 引擎交叉 | `runtime=cpp` + `engine=paddle` → **失败或自动改走 Python worker**（二选一钉死并测；推荐 **显式双 worker**，不失败用户） |
| API | 小模块：Swift 侧解析 + Python CLI 解析 +（可选）共享文档表 |
| 文档 | `cpp/README` / 用户文档：开发者如何强制 python/cpp |
| 单测 | 表驱动：各组合 → expected launcher |
| **不做** | 改 GUI 默认 spawn（归 06602/06605） |

---

### feat-06602 — macOS GUI 双 Worker 启动

**Oracle 行为：** 现 `PipelineClient` 只 spawn Python；6.6 扩展为按策略选二进制。

| 必须 | 说明 |
|---|---|
| C++ 启动 | `Process` 执行 `sublift_worker --socket … --engine vision|mock` |
| 路径解析 | 开发 cwd / build 产物 / Bundle `Helpers` 顺序文档化 |
| Paddle | 仍 `python -m sublift…` 现路径 |
| 握手 | 消费 capability；engines 不含所选 → 明确错误 |
| 取消 | 关 socket 语义不变（契约） |
| 测试 | 单元 mock Process；可选集成（VISION/mock） |
| **默认** | 本 feat 可先 **behind flag**（默认仍 python），06605 再翻；或直接按矩阵默认 cpp——须在 evidence 写明，避免半切换 |

**建议：** 06602 实现完整分支逻辑，**默认仍 python** 直至 06605，降低风险。

---

### feat-06603 — CLI cutover 路径

| 必须 | 说明 |
|---|---|
| 默认 | cutover 后：`sublift extract` vision/mock 走 C++（原生 cli 或 spawn worker/core） |
| 强制 | `--runtime python\|cpp` 与 env 对齐 06601 |
| Paddle | 仍 Python 实现路径 |
| 包装 | 若暂保留 `uv run sublift` 入口，须 **不**在 cutover 后强制用户装 uv 才能用 vision |
| 测试 | CLI 单测 mock runtime 解析；可选短视频 mock |

与契约 §2 对齐：**6.6 默认原生可执行**；Python CLI 保留 oracle/benchmark。

---

### feat-06604 — Cutover 门（正确性 + 运行时）

| 必须 | 说明 |
|---|---|
| Mock L2 | 既有 golden / detection_hash / 段边界等 **exact** 对照（C++ path vs 冻结 Oracle） |
| ROI / full | 像素与时间戳契约（6.3）在产品路径上再确认 |
| GT L3 | 固定素材：F1/precision/CER/usable/noise/empty **≥ 冻结水位**；产出报告路径进 `docs/reports/` 或 `debug/`（按仓库惯例） |
| Vision | L3 水位；**不**要求逐字 L0 |
| Cancel / restart | ≤1s / ≤5s **测量并写入报告**；超阈是否硬失败由本 feat 钉死（建议：cancel/restart 硬门，wall 中位 ×1.10 告警） |
| 脚本 | `scripts/` 下一键或半自动 runner；**禁止** C++ 重写整套 benchmark 指标 |
| ASan | Debug 可选 job 绿或记录豁免 |

**合并默认切换的前置条件：** 06604 报告 **通过** 或 waiver 经用户/维护者书面记录（DECISIONS）。

---

### feat-06605 — 默认翻转 + 回滚演练 + 收尾

| 必须 | 说明 |
|---|---|
| 默认 | GUI/CLI 产品默认 `runtime=cpp`（vision/mock） |
| 回滚 | `SUBLIFT_RUNTIME=python`（+ GUI 设置若有）测通；文档「一键回滚」 |
| Release note | CHANGELOG / README：默认 C++、paddle 例外、性能/质量摘要、回滚 |
| 跟踪 | phase6.json / feature-list / overview / progress 标 6.6 done |
| 保留 Python | 不删 worker；oracle/benchmark/paddle 路径仍可用 |
| 回归 | 06604 门在默认 cpp 下再跑一轮 smoke（或引用 06604 同报告） |

---

## 5. 建议实现落点

```text
docs/cpp/phase6.6-cutover.md          # 本文
docs/cpp/engine-matrix-and-cutover.md # 契约（不改冲突条款；实现注记可链回）

# Runtime 策略
apps/macos/.../RuntimePolicy.swift    # 或等价
src/sublift/runtime_policy.py         # CLI 共用文档化逻辑
# 或仅文档 + 双端各实现同一表

# GUI
apps/macos/.../PipelineClient.swift   # spawn C++ / Python 分支

# CLI
src/sublift/cli.py 和/或 cpp CLI
scripts/cutover/                      # 门禁 runner
docs/reports/phase6.6-cutover-*.md    # 门禁报告

# 测试
tests/... runtime_policy
apps/macos Tests launch
tests/ipc 默认 cpp smoke
```

### 启动参数对照（方向）

| Runtime | 命令形态 |
|---|---|
| Python | 现：`python -m sublift.ipc.server --socket … --engine …` |
| C++ | `sublift_worker --socket … --engine vision\|mock` |

Socket 分帧与消息 **不变**（6.5 已兼容）。

---

## 6. 风险与钉扎

| 风险 | 处理 |
|---|---|
| GUI 找不到 C++ 二进制 | 解析顺序 + 清晰错误；开发 README |
| cutover 后 paddle 用户困惑 | UI 文案 + 自动 Python worker |
| GT 波动 / 机差 | 同机对照；水位取自既有冻结报告；禁止偷偷降门 |
| 默认切换不可回滚 | 06605 强制回滚测；保留 Python ≥ 一小版本 |
| 半切换（CLI 已 cpp、GUI 仍 python） | 06605 清单双端一致；release note 写清 |
| 性能回归 | wall/RSS 告警阈；不单靠「感觉更快」 |

---

## 7. 6.6 完成定义

- [ ] `feat-06601`–`feat-06605` 全部 `done` + evidence  
- [ ] 默认：vision/mock → C++；paddle → Python；无静默 fallback  
- [ ] `SUBLIFT_RUNTIME=python`（或等价）可回滚  
- [ ] Cutover 门报告通过（或已归档 waiver）  
- [ ] cancel/restart 测有数字；init/CI 约定不破  
- [ ] 产品文档与 feature-list / phase6 跟踪一致  
- [ ] **未**删除 Python；**未**上 libav / 原生 paddle  

---

## 8. 与 6.7+ 边界

| 6.6 | 6.7+ |
|---|---|
| 默认 C++ worker；Python 可回滚 | 去嵌入 Python 产品依赖（条件成熟时） |
| 系统 ffmpeg | 随包 ffmpeg / 签名公证 / universal2 收尾 |
| paddle 走 Python | paddle-native 或产品放弃 paddle 默认 |
