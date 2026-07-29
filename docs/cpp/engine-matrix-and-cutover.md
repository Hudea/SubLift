# 引擎矩阵、Cutover 与回滚

> 状态：Phase 6.7 彻底完成；全引擎（vision / mock / paddle）均完成 Native C++ 支持与 Parity 门禁验证。  
> 解决冲突：文档不得再同时写「6.6 全面切 C++」与「不实现 Paddle」而不给矩阵。

## 1. 引擎 × Runtime 矩阵（冻结策略）

| engine | 6.0–6.5（双轨期） | 6.6 cutover 后默认 | **6.7+ paddle native** | 说明 |
|---|---|---|---|---|
| **vision** | Python 产品默认；C++ 实现后可开发者开关 | **C++ Worker** | 同左 | macOS 主路径 |
| **mock** | Python / C++ 均可测 | **C++ Worker**（测试与 CI） | 同左 | parity 主力 |
| **paddle** | **仅 Python Worker** | **仍走 Python Worker** | **C++ Worker（ONNX adapter 可用时）**；否则显式 Python | 设计见 [phase6.7-paddle.md](phase6.7-paddle.md) |
| （实现名）paddle-native | 无 | 无 | **`sublift_paddle` + ORT + PP-OCRv6** | 6.7 feat-067xx；不阻塞 6.6 |

### 1.1 明确禁止

- **禁止** 用户选 paddle 时静默落到 vision/mock。
- **禁止** C++ worker 对未知 engine 假装成功。
- GUI/CLI 只展示 **当前 runtime capability 声明的引擎**；若默认 runtime 是 C++ 且无 paddle，则：
  - 设置里 paddle 显示「需 Python runtime」或切换到 Python worker 启动；或
  - 选 paddle 时 **显式** spawn Python worker（双 worker 策略）。

### 1.2 推荐产品行为（6.6）

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
| `.app` 内 worker | `Contents/MacOS/sublift-worker` 或 `Contents/Helpers/` |
| OpenCV | 动态链接系统或 brew；或静态进 worker（体积/许可评估） |
| rpath | `@executable_path/../Frameworks` 预留 |
| 签名 / 公证 | 随 feat-025 / **6.8+**；符号 hidden 减少泄漏 |
| universal2 / 最低 macOS | 与 GUI 一致（当前 macOS 13+） |
| ffmpeg | 默认系统；随包另 feat |
| Paddle 模型 / ORT | 6.7 开发用系统 ORT + 用户缓存模型；随包模型 **6.8+** |

## 6. 与子阶段关系

| 子阶段 | 本矩阵相关动作 |
|---|---|
| 6.0 `feat-06005` | 冻结本文 + worker-ipc + 矩阵 |
| 6.4–6.5 | 实现 vision/mock C++；capability 上报 |
| 6.6 | vision/mock 默认 C++；paddle 显式 Python；双轨与回滚 |
| **6.7** | **paddle C++ adapter（ONNX）**；可用则 paddle→C++；见 [phase6.7-paddle.md](phase6.7-paddle.md) |
| 6.8+ | 去 Python 产品依赖 / 分发打包（**仅当**各引擎 native 足够或产品放弃 Python） |
