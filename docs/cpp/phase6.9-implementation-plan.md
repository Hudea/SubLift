# Phase 6.9 — Native 产品架构整理 · 实施计划

> 子阶段编码：`S = 9` → feature 前缀 `feat-069xx`  
> 目标态设计：[`phase6.9-native-product-architecture.md`](phase6.9-native-product-architecture.md)  
> 任务跟踪：`docs/phases/phase6.json`  
> 前置：Phase 6.8 全部完成（feat-06801–06807）；`main` / 本分支基线 `99e361e`  
> 分支：`refactor/native-product-architecture`（worktree：`SubLift_CPP`）  
> 状态：**计划已冻结；实现按 feature 串行**

---

## 1. 最终 Goal

把仓库从「能跑且已 cutover 的 C++ 产品路径」整理成：

> **依赖方向清晰、Target 边界干净、目录与 public API 可读、资源由 Native 自洽发现、
> 正式产品不依赖 Python、Swift/CLI 共用薄客户端 + C++ Worker，且 6.8 质量/性能门零退化。**

「优美」在本阶段的可操作定义：

| 维度 | 完成时的外观 |
|---|---|
| **依赖** | `core → pipeline → application → worker`；adapters 只实现 Ports；protocol 无 OCR/Vision |
| **Target** | 无 `sublift_ipc` 巨石库；CLI 不链接 ORT/OpenCV/Vision |
| **目录** | `src/adapters/*`、`protocol/`、`application/`、`diagnostics/` 与文档一致 |
| **Public API** | `include/sublift/{core,ports,pipeline,protocol}`；Paddle 产品头仅 engine/options |
| **资源** | `ResourceLocator + ModelBundle`：probe ≡ construct；SHA fail-closed |
| **产品路径** | GUI 与正式 CLI 只 spawn C++ Worker；Python 仅 Oracle/benchmark/dev |
| **分发** | 可构建的 macOS 布局 + 相对 rpath + notices；干净环境可提取 SRT |

Phase 6.9 **不**重写已验收的 OCR 数值路径，也**不**删除 Oracle。

---

## 2. 当前差距（冻结基线，来自 2026-07-30 代码探索）

### 2.1 已具备

- Worker 进程边界 + UDS/JSON IPC；Swift / Native CLI 已能 spawn Worker
- `sublift_core` / `ffmpeg` / `paddle` / `vision_macos` / `worker` / `cli` / `test_support`
- Ports 头文件：`ocr.hpp`、`extractor.hpp`、`detector.hpp`
- ORT build-tree 复制 + `@loader_path/../lib`（开发态）
- 6.8 全门：stage / quality / perf / cutover / 720s / cancel / restart

### 2.2 关键债务

| 债务 | 现状 |
|---|---|
| **巨石 `sublift_ipc`** | framing + protocol + bridge + connection + engine_factory；PUBLIC 链接全部 adapters |
| **core ⊃ pipeline** | OpenCV 开启时 signature/changepoint/pipeline 在 `sublift_core` |
| **protocol 不纯** | `protocol.cpp` 引用 vision 做 capability 猜测 |
| **CLI 链接面过大** | `sublift_cli` → `sublift_ipc` → ORT/OpenCV/Vision |
| **Paddle 公共面过大** | `paddle.hpp` 含 stage tensor / test override / dump |
| **模型发现** | `~/.cache/sublift/rapidocr-models`；首次准备依赖 Python rapidocr |
| **Native CLI 引擎不全** | 仅 mock/vision；paddle 仍指向 Python CLI |
| **无正式 `.app` 打包** | 无 packaging 脚本；模型/ffmpeg 未随包 |
| **Python 仍产品 fallback** | `paddle_override` + GUI/Python CLI 可 spawn Python Worker |

---

## 3. 全局不变量（全 feature 适用）

1. **不改** signature / changepoint / timeline / dedupe / line_select / Paddle Det·Cls·Rec **数值语义**（目录移动与 target 拆分除外的机械修复）。
2. **不更新 golden** 作为目录/target 整理的附带产物；任何 golden 变更必须独立 feature、带 Oracle 指纹与说明。
3. **6.8 门不退化**：`check_paddle_det_parity` / `rec_parity` / `gate` / `perf` / `cutover` 与 cancel/restart/720s 阈值。
4. **fallback 观察期结束前**不得删除产品 Python 回滚路径（见 feat-06913）。
5. 默认 `./init.sh` **不**塞入长视频 / 公证 / 下载模型；发布门走独立脚本或 env。
6. 一次只做一个 `feat-069xx`；后一个不得用「后续补门」绕过前一个验收。
7. 物理移动与行为修改 **禁止同 commit**；移动后仅允许 include/CMake 机械修复。
8. 保留仓库根唯一 `cpp/`；不把 CMake 提升到仓库根。
9. 不引入 Boost / Qt / 重型 DI 框架；不默认开启无界并发 OCR。
10. public headers 不暴露 `cv::Mat`、Ort 类型、Objective-C、nlohmann 作为 **产品** API（protocol 允许私有使用 JSON）。

---

## 4. 波浪与依赖总图

```text
Wave A — 结构优美（零行为）
  feat-06901 protocol target 纯度
    └─ feat-06902 application + 拆解 sublift_ipc
        └─ feat-06903 pipeline 从 core 拆出
            └─ feat-06904 薄 CLI 链接面
                └─ feat-06905 机械目录 + include 布局
                    └─ feat-06906 Paddle 公共面收缩

Wave B — Native 资源与单一产品路径
  feat-06907 ModelBundle / ResourceLocator
    └─ feat-06908 Capability 单一来源
        └─ feat-06909 Native CLI 产品 parity（含 paddle）

Wave C — 分发与去 Python 产品依赖
  feat-06910 macOS product bundle 布局
    └─ feat-06911 签名 / 公证 / 干净机 Gatekeeper
        └─ feat-06912 Python-free 产品门

Wave D — 退役与收口
  feat-06913 Fallback 退役 + 文档/矩阵终态
```

```text
feat-06807 (done)
  └─ 06901 → 06902 → 06903 → 06904 → 06905 → 06906
                                              │
                              06907 → 06908 → 06909
                                              │
                              06910 → 06911 → 06912 → 06913
```

**并行约束：** Wave A 完成前不开始 06910 打包（避免在错误布局上固化安装路径）。  
**06907 可与 06905 尾部重叠的条件：** 仅当 ResourceLocator 不依赖最终目录路径字符串时；默认仍串行。

Linux / Windows 分发、Named Pipe、`src/sublift` → `tools/python/` 物理迁移 = **6.10+**（本计划仅列非目标与接口预留）。

---

## 5. Feature 详述

### feat-06901 — `sublift_protocol` Target 与协议纯度

| 字段 | 内容 |
|---|---|
| **一句话** | 把 framing + protocol DTO/JSON 收成独立 target；协议层不再探测 Vision/OCR。 |
| **模块** | `cpp/src/worker/framing.*`、`protocol.*`；`cpp/src/worker/CMakeLists.txt`；相关 worker 单测 |
| **依赖** | feat-06807 |
| **允许改** | 新建 `sublift_protocol` STATIC；`sublift_ipc`（或后续 application）链接它；删除 `protocol.cpp` 对 `vision.hpp` 的 include 与默认引擎猜测 |
| **禁止改** | framing 字节序、DTO 字段名、JSON schema 行为；golden |

**Subtasks**

1. CMake 声明 `sublift_protocol`（sources: framing + protocol only）。
2. 能力/引擎列表改为由 **Composition Root / factory 注入**（与 hello 同源），**不**在
   protocol 内调用 `is_vision_available()` 或任何 adapter probe。
3. **零行为硬约束：** 同一构建选项下，bye/hello 对外引擎列表与 capability 必须与
   拆分前 **逐项一致**（禁止用空列表或 placeholder 冒充纯度）。
4. 现有 `worker_framing_test` / `worker_protocol_test` 继续通过。

**验收条件**

- [ ] `sublift_protocol` 的 `target_link_libraries` **不含** `sublift_paddle` / `sublift_vision_macos` / `sublift_ffmpeg` / OpenCV / ORT。
- [ ] `nm`/`otool` 或 CMake graph 证明 protocol 不依赖 Vision framework。
- [ ] 同配置 handshake：引擎列表/capability 与拆分前 snapshot 一致（测试或手工对照）。
- [ ] `ctest -R 'worker_framing|worker_protocol'` 全绿。
- [ ] `./init.sh` 日常门 10/10。
- [ ] 无 golden 变更。

---

### feat-06902 — `sublift_application` + 拆解 `sublift_ipc`

| 字段 | 内容 |
|---|---|
| **一句话** | Bridge/Job 生命周期进入 `sublift_application`；Worker 成为 Composition Root；去掉巨石 `sublift_ipc` 名称与 PUBLIC 全量 fan-out。 |
| **模块** | `bridge.*`、`connection.*`、`engine_factory.*`、`main.cpp`、CMake |
| **依赖** | feat-06901 |
| **允许改** | Target 图与链接方向；符号命名；composition 装配位置 |
| **禁止改** | path/frame mode 语义、cancel 时序、IPC 消息语义 |

**Subtasks**

1. `sublift_application`：ExtractJob / BridgeHandler / 进度与取消（依赖 protocol + pipeline ports + 抽象 Ports）。
2. `engine_factory` 仅被 Worker main（Composition Root）使用，不进入 protocol。
3. `sublift_worker` 链接 application + adapters；删除或降级 `sublift_ipc` 为过渡别名（最终删除）。
4. connection 只依赖 application 接口，不反向依赖具体 OCR 头文件（能抽接口则抽）。

**验收条件**

- [ ] 依赖图符合目标：`worker → application → protocol|pipeline|ports`；adapters 不依赖 worker。
- [ ] IPC E2E（pytest 非 integration 中相关用例 + worker cancel/path_mode 测试）全绿。
- [ ] mock/vision/paddle（若构建开启）`--probe-engine` 与 start_job 行为与 6.8 一致。
- [ ] `./init.sh` PASS；无 golden 变更。

---

### feat-06903 — `sublift_pipeline` 从 `sublift_core` 拆出

| 字段 | 内容 |
|---|---|
| **一句话** | 纯领域（models/config/image/…）留在 core；流式 Pipeline 与 signature/changepoint 进入 `sublift_pipeline`。 |
| **模块** | `cpp/src/core/pipeline.cpp`、`signature.cpp`、`changepoint.cpp` 及对应头；CMake |
| **依赖** | feat-06902（application 已能依赖 pipeline target） |
| **允许改** | Target 边界；OpenCV PRIVATE 仅挂在需要它的 target |
| **禁止改** | 算法与 golden |

**验收条件**

- [ ] `sublift_core` 在无 OpenCV 时仍可独立构建（与现策略一致或更清晰）。
- [ ] `sublift_pipeline` 仅依赖 core + Ports 抽象，不实例化 ffmpeg/OCR。
- [ ] pipeline / signature / changepoint unit + parity 测试全绿。
- [ ] 无 golden 变更；`./init.sh` PASS。

---

### feat-06904 — 薄 Native CLI 链接面

| 字段 | 内容 |
|---|---|
| **一句话** | `sublift_cli` 只链接 protocol + 进程启动/UDS client，不链接 paddle/vision/ffmpeg/OpenCV/ORT。 |
| **模块** | `cpp/src/cli/`、CMake；必要时抽出 `sublift_client` 小库 |
| **依赖** | feat-06902 |
| **允许改** | CLI 与 worker 之间的 client 辅助代码拆分 |
| **禁止改** | CLI 业务：仍 spawn Worker，不实现 in-process OCR |

**验收条件**

- [ ] `otool -L build/.../sublift`（或等价）**不**出现 `libonnxruntime`、OpenCV dylib（允许系统 lib）。
- [ ] `sublift extract --engine mock`（及可用时 vision）经 Worker 跑通。
- [ ] `./init.sh` PASS。

---

### feat-06905 — 机械目录与 include 布局

| 字段 | 内容 |
|---|---|
| **一句话** | 物理目录对齐目标树；public include 分层；允许短期兼容 re-export。 |
| **模块** | 整个 `cpp/src`、`cpp/include`、diagnostics、CMake、测试 include |
| **依赖** | feat-06901–06904（targets 已存在） |
| **允许改** | `git mv`、include 路径、CMake `target_sources` 路径；可选 `include/sublift/*.hpp` 转发到 `core/` 等 |
| **禁止改** | 任何算法/默认参数/字符串语义 |

**目标树（与架构文档一致，允许分两次提交完成）：**

```text
cpp/
├── include/sublift/{core,ports,pipeline,protocol}/
├── src/{core,pipeline,application,protocol,adapters/{ffmpeg,paddle,vision_macos},worker,cli}/
├── diagnostics/paddle_trace/
├── tests/{unit,contract,integration,parity}/   # 可分步；至少不动 parity 路径语义
└── packaging/   # 空目录或 README 占位即可，实现在 06910
```

**验收条件**

- [ ] 映射表与 `phase6.9-native-product-architecture.md` §6 一致（或文档同步最终差异）。
- [ ] 全量相关 ctest 绿；`./init.sh` PASS。
- [ ] `git diff` 无 golden / fixture 内容变化（除路径字符串若绝对必要且行为不变——默认零变更）。
- [ ] 无产品行为日志差异（同一 mock 短片 entry 数一致）。

---

### feat-06906 — Paddle 产品公共面收缩

| 字段 | 内容 |
|---|---|
| **一句话** | 产品 `paddle.hpp` 仅保留 options/capabilities/engine；trace/override 进入 diagnostics 或 private。 |
| **模块** | `include/sublift/paddle.hpp`、`src/adapters/paddle/`、`diagnostics/`、parity/trace 工具 |
| **依赖** | feat-06905（目录已稳）或 06905 前半完成后 |
| **允许改** | 头文件拆分、include 迁移；trace 工具改 include |
| **禁止改** | recognize 默认路径、batch/thread 产品默认、stage 数值 |

**验收条件**

- [ ] 产品 Worker/CLI 编译单元不 `#include` stage tensor 类型。
- [ ] `sublift_paddle_trace` + `check_paddle_*` / stage parity **仍可**访问 diagnostics API。
- [ ] Paddle unit ctest 与（若环境具备）det/rec offline check 绿。
- [ ] 无 SRT / stage golden 变更。

---

### feat-06907 — ModelBundle + ResourceLocator

| 字段 | 内容 |
|---|---|
| **一句话** | 统一模型/ORT/ffmpeg 查找：override → 安装资源 → 校验缓存 →（可选）下载 → 明确错误；probe ≡ construct。 |
| **模块** | 新 `ResourceLocator` / `ModelBundle`；`paddle_models.*`；Worker probe；文档 |
| **依赖** | feat-06906 推荐（public API 已稳）；最低 06902 |
| **允许改** | 解析器实现、manifest schema、SHA 校验、错误码 |
| **过渡兼容** | 默认仍识别 `~/.cache/sublift/rapidocr-models` + 现文件名，避免开发机立刻断裂 |
| **禁止改** | 模型权重文件本身；产品默认 tier（small）除非另立决策 |

**Manifest 最低字段**（架构文档 §10.2）：schema、tier、det/cls/rec/dict 文件名与 SHA256、兼容 ORT、默认 thread/batch、许可、最低产品版本。

**验收条件**

- [ ] `is_paddle_available()` / `--probe-engine paddle` / `PaddleOcrEngine` 构造使用**同一**解析函数。
- [ ] 半下载/半拷贝模型目录 fail-closed（不可被 probe 判真）。
- [ ] 单测覆盖：override、缺文件、SHA 失败、合法 cache。
- [ ] 现有开发机 rapidocr cache 布局仍可用（兼容路径）。
- [ ] 不强制把 live quality 门塞进 init；可选 `SUBLIFT_INIT_PADDLE=1` 文档说明。
- [ ] `./init.sh` PASS。

---

### feat-06908 — Capability 单一事实来源

| 字段 | 内容 |
|---|---|
| **一句话** | hello/bye/capabilities 只由 Composition Root 根据真实 Adapter+资源计算。 |
| **模块** | Worker main、engine_factory、protocol mapper、Swift/Python 解析侧（只读兼容） |
| **依赖** | feat-06907 |
| **禁止改** | 客户端协议字段名破坏性变更（若需扩展用版本字段） |

**验收条件**

- [ ] 协议层无独立「猜引擎」逻辑。
- [ ] capability 与真实 start_job 失败模式一致（有 capability 则构造路径一致）。
- [ ] Swift/Python handshake 测试绿；IPC E2E 绿。

---

### feat-06909 — Native CLI 产品 parity

| 字段 | 内容 |
|---|---|
| **一句话** | 正式 CLI 支持 paddle（经 Worker）；身份日志与 GUI 一致；Python CLI 文档标为 dev/oracle。 |
| **模块** | `cpp/src/cli/main.cpp`、README、ARCHITECTURE、Runtime 文档；可选 Python CLI 警告 |
| **依赖** | feat-06904 + feat-06908 |
| **允许改** | CLI flags、帮助文案、identity 日志 |
| **禁止改** | 为 CLI 增加 in-process OCR 捷径 |

**验收条件**

- [ ] `build/.../sublift extract --engine paddle` 在 capability 可用时产出非空/与预期一致的 SRT（短片 smoke）。
- [ ] 日志含 runtime=cpp、engine、model tier、stable/error（与 Worker 一致）。
- [ ] README 明确：产品 CLI = Native；`uv run sublift` = dev/oracle/rollback。
- [ ] `./init.sh` PASS。

---

### feat-06910 — macOS Product Bundle 布局

| 字段 | 内容 |
|---|---|
| **一句话** | 可重复构建 `SubLift.app`（或等价 prefix）：Worker、ORT、模型、ffmpeg、NOTICE、相对 rpath。 |
| **模块** | `cpp/packaging/macos/`、CMake install/bundle 规则、ResourceLocator 安装路径、`THIRD_PARTY_NOTICES.md` 扩充 |
| **依赖** | feat-06907、feat-06909 |
| **允许改** | 打包脚本、install 规则、bundle 内路径 |
| **禁止改** | 为打包放宽 SHA/质量门 |

**验收条件**

- [ ] 布局符合架构文档 §13（Helpers/worker、Frameworks/ORT、Resources/models+manifest、ffmpeg）。
- [ ] `otool -L` 对 Worker/App **无** Homebrew / `.venv` / 绝对开发机路径依赖（允许系统框架）。
- [ ] 使用 bundle 内模型+ORT，断网可完成至少一次 mock 或 paddle smoke（按产品定义）。
- [ ] notices 覆盖 ORT、OpenCV、ffmpeg、Paddle/RapidOCR 算法来源等已用依赖。
- [ ] 打包脚本文档化；默认 init 不执行公证。

---

### feat-06911 — 签名、公证与干净机 Gatekeeper

| 字段 | 内容 |
|---|---|
| **一句话** | Developer ID 签名、notarization、staple；另一台干净 Mac / 干净用户环境 Gatekeeper 通过。 |
| **模块** | packaging 脚本、CI/本地 runbook、报告 |
| **依赖** | feat-06910 |
| **注意** | 需要证书与 Apple 账号；若环境不具备，feature 可拆「脚本就绪」与「实机公证证据」两段，但不得声称完成而未 staple。 |

**验收条件**

- [ ] `codesign --verify --deep --strict` 通过。
- [ ] notarization + staple 成功（或书面阻塞：缺证书，status=blocked + 复现步骤）。
- [ ] 干净环境启动 App / CLI helper，完成真实短视频 → SRT（paddle 或 vision 按默认引擎）。
- [ ] 证据写入 `docs/reports/` 与 phase6.json。

---

### feat-06912 — Python-free 产品门

| 字段 | 内容 |
|---|---|
| **一句话** | 正式 fail-closed 门：产品路径零 Python 进程；与 6.8 质量/性能/长流门对齐。 |
| **模块** | `scripts/parity/check_python_free_product.py`（名可微调）、packaging 验收、文档 |
| **依赖** | feat-06910（06911 若 blocked 可由「未签名 bundle + PATH 隔离」降级验收，但报告必须标明） |
| **门检查项** | 见架构文档 §19 清单 |

**验收条件**

- [ ] 脚本在 `PATH` 无 python/uv、无 `.venv` 优先的环境执行产品 binary。
- [ ] 全程无 `python`/`uv` 子进程（process 树断言）。
- [ ] 6.8 cutover 级 smoke：短片 SRT、cancel≤1s、restart≤5s；长流门可 env 触发。
- [ ] 不削弱 `check_paddle_cutover` / quality / perf（可引用既有报告 + 抽测）。
- [ ] 报告输出 `/tmp` 或 `debug/`，不污染 docs 除非归档。

---

### feat-06913 — Fallback 退役与终态文档

| 字段 | 内容 |
|---|---|
| **一句话** | 观察期结束后，产品侧不再因 paddle 不可用静默启动 Python；给出可操作 Native 错误；更新矩阵与 ADR。 |
| **模块** | `runtime.py`、`RuntimePolicy.swift`、`PipelineClient`、引擎矩阵、README、DECISIONS |
| **依赖** | feat-06912 + **至少一个小版本观察期**（自 06807 cutover 起算；文档写明日期门槛） |
| **保留** | Python Oracle、parity gates、`SUBLIFT_RUNTIME=python` **开发强制** 可保留为 dev-only（产品 GUI Release 构建可编译剔除） |

**验收条件**

- [ ] 产品默认矩阵：cpp unavailable → **错误**，不 spawn Python Worker（Release 产品配置）。
- [ ] Dev 回滚路径有明确文档与测试，不与产品默认混淆。
- [ ] 引擎矩阵 / ARCHITECTURE / REQUIREMENTS / ADR 已更新。
- [ ] phase6.json evidence + feature-list `phase6.post-cutover` = done。
- [ ] 架构文档 §19 勾选清单全部满足或逐条引用证据。

---

## 6. 每 feature 的标准验证包

| 门 | 何时 |
|---|---|
| `./init.sh` | **每个** feature 合并前 |
| 相关 ctest 子集 + 全量 Debug ctest | 每个 C++ 结构 feature |
| Swift `swift test`（若改 Runtime/PipelineClient） | 06908/06909/06913 |
| `check_paddle_det_parity` / `rec_parity`（offline 优先） | 06905–06907、06910 后抽测 |
| `check_paddle_gate` / `perf` / `cutover` | 06907 后重大资源变更；06910/06912 强制 |
| golden diff = empty | Wave A 全部 |

---

## 7. 非目标（本子阶段不做）

| 不做 | 去向 |
|---|---|
| 机械翻译全部 Python → C++ | 永不作为目标 |
| 删除 Worker IPC / Swift 直链 C++ | 仅当 IPC 被证明瓶颈时另立 feature |
| 在 C++ 重写整套 benchmark 指标 | Python dev tools |
| Linux/Windows 安装包与 Named Pipe 完整实现 | **6.10+**（protocol/application 保持可移植） |
| `src/sublift` → `tools/python/` 大搬家 | 独立 feature；06913 之后 |
| 放宽 ORT/model SHA 或 SRT exact 门 | 禁止 |
| 无界并行 OCR/Pipeline | 禁止 |
| 把公证/长视频塞进默认 `./init.sh` | 禁止 |

---

## 8. 风险与缓解

| 风险 | 缓解 |
|---|---|
| 拆 target 导致 init 变慢 | 保持单次 ctest；重门独立脚本 |
| include 大搬家编译崩溃 | 先 target 后目录；re-export 头过渡 |
| ResourceLocator 破坏开发机 cache | 兼容 rapidocr 路径；SHA 先 warn 后 enforce 可两阶段（仍在 06907 内用 subtask 表达） |
| 公证缺证书 | 06911 status=blocked，不阻塞 06910 完成声明 |
| Fallback 过早删除 | 06913 硬依赖观察期 + 06912 |
| CLI 变薄后测试链接断裂 | tests 继续链 application/adapters；CLI 单独验 otool |

---

## 9. 跟踪与文档维护

| 产物 | 职责 |
|---|---|
| 本文 | 实施顺序、feature 边界、验收 |
| [`phase6.9-native-product-architecture.md`](phase6.9-native-product-architecture.md) | 目标态架构（不变则不改；实现后同步映射表） |
| `docs/phases/phase6.json` | feat-069xx 状态与 evidence **唯一事实来源** |
| `feature-list.json` → `phase6.post-cutover` | 项目级 covers |
| `docs/DECISIONS.md` | 架构决策 ADR（如 ResourceLocator 顺序、fallback 退役日） |
| `progress.md` | 导航（短） |

**开始实现：** 只选一个 `status: not-started` 的 feature → 改代码 → 跑验收 → 写 evidence → 再开下一个。

---

## 10. 建议执行节奏

| 节奏 | Features | 预期产出 |
|---|---|---|
| 第 1 段 | 06901–06904 | Target 图干净、CLI 变薄 |
| 第 2 段 | 06905–06906 | 目录与 API 可读 |
| 第 3 段 | 06907–06909 | Native 资源 + 正式 CLI |
| 第 4 段 | 06910–06912 | 可分发、可证明无 Python |
| 第 5 段 | 06913 | 退役 fallback，关闭 6.9 |

每段结束后可打 tag：`phase6.9-waveA` 等（可选）。

---

## 11. 完成定义（整个 6.9）

当且仅当：

1. feat-06901–06913 均为 `done`（06911 若永久无证书则需用户书面降级范围）；  
2. 架构文档 §19 Python-free 清单全部有 evidence；  
3. 目标依赖图与目录在 README/architecture 中与代码一致；  
4. 6.8 质量/性能/长流门无回归报告；  
5. `feature-list.json` 中 `phase6.post-cutover` = `done`。

---

## 12. Key Decisions（本计划冻结）

| 决策 | 选择 | 理由 |
|---|---|---|
| 先 target 后目录 | 是 | 降低移动风险；依赖错误先在链接期暴露 |
| CLI 走 Worker | 是 | GUI/CLI 单一产品路径 |
| 模型 small 随包 | 是（06910） | 干净机断网可用；其它 tier 可后置下载 |
| 观察期后再删 fallback | 是 | 与 06807 承诺一致 |
| Linux/Windows 完整包 | 6.10+ | 先收口 macOS 产品优美与可证明 |
| 不提升 `cpp/` 到仓库根 | 是 | 多语言仓合理布局 |

---

## 13. Open Questions（实现前可默认建议）

| # | 问题 | 默认建议 | 影响 feature |
|---|---|---|---|
| Q1 | OpenCV 静态 vs 动态进 bundle | 先动态 + 相对 rpath，体积/许可矩阵后再冻结静态 | 06910 |
| Q2 | 06907 SHA 是否第一天 enforce | 第一天校验 manifest 若存在；无 manifest 的 rapidocr cache 兼容期 warn，06910 随包强制 | 06907/06910 |
| Q3 | 产品 GUI 是否与 CLI 同一次 bundle | 同一 `.app`：MacOS GUI + Helpers/worker | 06910 |
| Q4 | 观察期长度 | 自 06807 起 ≥1 个 minor 或 ≥2 周真实使用（先到为准） | 06913 |
| Q5 | ModelManager vs ResourceLocator 命名 | 采用 `ResourceLocator` + `ModelBundle`；`IModelStore` 若需要则为 Port 别名，不另起平行类型 | 06907 |
| Q6 | 06912 与 06911 关系 | 06912 可用「未签名但 PATH 隔离的 bundle」验收进程树；§19 签名项归 06911 evidence | 06911/06912 |

用户可在实现 Wave B/C 前修改上表；未修改则按默认建议执行。
