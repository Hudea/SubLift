# 引擎矩阵、Cutover 与回滚

> 状态：Phase 6.8 全引擎 C++ cutover 与 Phase 6.9 开发架构收口完成。
>
> Native CLI / Swift 开发构建默认使用 C++ Worker；Python 仅通过显式 runtime 作为
> Oracle、benchmark 与开发回滚。`.app` 分发、签名和发布 artifact Python-free 门按
> ADR-0030 后置，本文不声明已发布独立安装包。

## 1. 引擎 × Runtime 矩阵（终局策略）

| engine | 6.0–6.5（双轨期） | 6.6 cutover | 6.7 MVP | **6.8 Final Cutover** | 说明 |
|---|---|---|---|---|---|
| **vision** | Python 产品默认 | **C++ Worker** | **C++ Worker** | **C++ Worker** | macOS 主路径 |
| **mock** | Python / C++ | **C++ Worker** | **C++ Worker** | **C++ Worker** | parity 主力 |
| **paddle** | 仅 Python Worker | 仍走 Python Worker | C++ adapter | **C++ Worker 产品默认**（可用时）；否则显式 Python | 禁止静默改 vision/mock |
| （实现名）paddle-native | 无 | 无 | `sublift_paddle` MVP | **`sublift_paddle` 完整 DB/Cls/Rec Native** | 正式产品路径 |

### 1.1 明确禁止

- **禁止** 用户选 paddle 时静默落到 vision/mock。
- **禁止** C++ worker 对未知 engine 假装成功。
- GUI/CLI 只展示 **当前 runtime capability 声明的引擎**；若默认 runtime 是 C++ 且无 paddle，则：
  - 返回可操作的 Native capability 错误；
  - 只有用户/开发者显式指定 `runtime=python` 时才启动 Python Worker。

### 1.2 历史过渡行为（6.6–6.8 hardening）

**双 Worker 按引擎选择（推荐）：**

```text
engine in {vision, mock} → 启动 sublift-worker (C++)
engine == paddle         → 启动 Python ipc server（现路径）
```

- CLI：`sublift` 原生解析参数后同样分支；或 `sublift extract --runtime python|cpp` 强制。
- 开发者双轨：环境变量 / 配置 `SUBLIFT_RUNTIME=python|cpp`（**6.6**：cpp 时 paddle 强制 Python worker，不改引擎）。

**6.7 推荐产品行为（paddle native 可用后）：**

```text
engine in {vision, mock} → C++ worker（同 6.6）
engine == paddle && C++ paddle available → C++ worker（sublift_paddle）
engine == paddle && C++ paddle unavailable → Python worker（显式 override，提示安装/构建）
```

- **禁止** 不可用时静默改为 vision/mock。
- `SUBLIFT_RUNTIME=python` 仍可强制全引擎走 Python（oracle / 回滚）。

**6.8 hardening 期间（`feat-06801` 实施后）：**

```text
engine in {vision, mock} → C++ worker（不变）
engine == paddle，未显式指定 runtime → Python worker（安全默认）
engine == paddle，显式 runtime=cpp 且 available → C++ worker（experimental）
engine == paddle，显式 runtime=cpp 但 unavailable → 明确错误或显式 Python override
```

只有 `feat-06803`–`feat-06806` 的 stage、质量、性能和稳定性门全部通过，
`feat-06807` 才允许恢复 6.7 的自动 C++ 默认。

**6.8 Final Cutover（历史切换点）：**

```text
engine in {vision, mock} → C++ worker
engine == paddle && C++ paddle available → C++ worker (stable)
engine == paddle && C++ paddle unavailable → Python Paddle (paddle_override)
explicit/env runtime=python → Python worker（回滚 / Oracle）
```

CLI、Worker 与 GUI 均显示最终 runtime；Paddle 同时显示 `PP-OCRv6-small` 和
`stable|fallback`。Swift availability 直接执行目标 Worker 的
`--probe-engine paddle`，避免 GUI 与实际 capability 漂移。

**6.9 开发架构终态（当前）：**

```text
engine in {vision, mock} → C++ worker
engine == paddle && C++ paddle available → C++ worker (stable)
engine == paddle && C++ paddle unavailable → 明确错误（fail-closed）
explicit/env runtime=python → Python worker（Oracle / 开发回滚）
```

不再存在自动 `paddle_override`；显式 Python 路径不代表未来发布 artifact 包含 Python。

**非 macOS：** 默认引擎候选为 paddle（6.7 native 或 Python）；无 Vision。Linux 至少可 `ENABLE_PADDLE=ON` 构建（见 6.7 设计）。

## 2. CLI 形态

| 阶段 | CLI |
|---|---|
| 6.0–6.5 | 产品 CLI 仍为 Python；可选 `cpp/apps/cli` 开发者入口 |
| 6.6 | **默认原生 `sublift` 可执行文件** 调 core；Python `uv run sublift` 保留为 oracle/benchmark |
| 包装 | 可用薄包装脚本调原生二进制，但 **不得** 在 cutover 后仍强制 uv 才能用 vision |

## 3. Cutover 门（质量 + 运行时 + 回滚）

### 3.1 正确性

| 门 | 要求 |
|---|---|
| Mock L2 | detection_hash、段边界、OCR 调用次数、代表帧 ts **exact** vs 冻结 Oracle |
| full/ROI | 像素管线与时间戳与 Oracle 一致（ROI 契约同 Phase 4） |
| GT L3 | F1/precision/CER/usable/noise/empty **不低于** 冻结水位（同机对照报告） |
| Vision | L3 水位；不要求与某次 Vision 逐字 identical |

### 3.2 运行时 / UX

| 门 | 要求 |
|---|---|
| 首帧 / 首条 | 不劣于 Oracle 报告中的基线（记录数值，允许文档化噪声） |
| wall / RSS | 同负载不 **显著退化**（建议 wall 中位 ≤ Oracle×1.10 作为告警阈；硬性阈值 cutover PR 写死） |
| cancel | ≤ **1s** |
| restart | ≤ **5s**，无串扰 |
| 长流 | ≥10min 样本资源稳定（对齐 feat-040 精神） |

### 3.3 工程

| 门 | 要求 |
|---|---|
| ASan/UBSan | Debug/CI job 全绿（macOS） |
| 编译矩阵 | macOS 必选；Linux 至少 **core + ffmpeg + mock** 可编译（Vision off） |
| Benchmark 接入 | C++ 结果可喂现有 Python benchmark 脚本，**不**分叉第二套指标实现 |
| 双轨开关 | `python\|cpp` 文档化；默认切换有 release note |

### 3.4 回滚

| 条件 | 动作 |
|---|---|
| GT 任一硬指标跌破水位 | **阻止** 默认 cutover 合并 |
| 线上/开发者默认 C++ 后发现严重回归 | 配置/环境变量切回 Python worker；保留 C++ 为 opt-in |
| Paddle 路径回归 | 仅影响 paddle 分支；不自动回滚 vision C++ |
| 回滚时限 | cutover 后至少保留 Python worker **一个小版本周期** 可一键切回 |

回滚开关示例：

```text
SUBLIFT_RUNTIME=python|cpp   # 默认 cpp（cutover 后）
# 或 UserDefaults / CLI --runtime
```

## 4. Candidate 接入 Benchmark（不复制诊断）

```text
C++ worker/cli
  → 写出 entries.jsonl / srt / 可选中间 JSONL
  → 现有 benchmark 对齐脚本（Python）读入并打 F1/CER…
```

或：

```text
benchmark runner --runtime cpp -- 内部 spawn C++ 与 python 对照
```

禁止在 C++ 内重写整套 cluster 诊断；需要时只加 **导出适配层**。

## 5. 部署模型（P2 登记，反向约束 CMake）

| 项 | 倾向 |
|---|---|
| `.app` 内 worker | 未来发布设计：`Contents/MacOS/` 或 `Contents/Helpers/` |
| OpenCV | 动态链接系统或 brew；或静态进 worker（体积/许可评估） |
| rpath | `@executable_path/../Frameworks` 预留 |
| 签名 / 公证 | ADR-0030 后置到未来发布阶段 |
| universal2 / 最低 macOS | 与 GUI 一致（当前 macOS 13+） |
| ffmpeg | 默认系统；随包另 feat |
| Paddle 模型 / ORT | 6.8 build tree 复制已验收 ORT并用相对 rpath；开发期使用缓存模型；正式随包后置 |

## 6. 与子阶段关系

| 子阶段 | 本矩阵相关动作 |
|---|---|
| 6.0 `feat-06005` | 冻结本文 + worker-ipc + 矩阵 |
| 6.4–6.5 | 实现 vision/mock C++；capability 上报 |
| 6.6 | vision/mock 默认 C++；paddle 显式 Python；双轨与回滚 |
| **6.7** | **paddle C++ Native MVP（ONNX）**；当前代码可用则 paddle→C++；见 [phase6.7-paddle.md](phase6.7-paddle.md) |
| **6.8** | 安全路由 → stage parity → 多源质量门 → 性能 → 重新 cutover；见 [phase6.8-paddle-hardening.md](phase6.8-paddle-hardening.md) |
| 6.9 | Target/目录/Ports/Adapters/ResourceLocator/Native CLI 开发架构收口 |
| 未来发布 | bundle、依赖随包、签名公证、最终 artifact Python-free 门（ADR-0030） |

## 7. Phase 6.8 最终实测

- canonical 120s：Python/C++ wall median `50.700/45.407s`，ratio `0.8956x`；
  进程树 RSS `1864.406/1706.297MiB`，ratio `0.9152x`。
- 3 来源 614.272s：Python/C++ 逐源 SRT SHA exact，全部质量指标 delta=0。
- rollback：默认 C++ `4.615s` → 强制 Python `5.097s` → 默认 C++ restart
  `4.491s`，10 entries 与 SRT SHA 全部 exact。
- 连续长流：720s source，C++ wall `15.522s`，40 entries。
- 真实 Paddle IPC：in-flight cancel `2.8ms`；同 Worker restart readiness `0.1ms`。
- 报告：[phase6.8-paddle-cutover.md](../reports/phase6.8-paddle-cutover.md)。
