# SubLift 测试全景与运行指南 (TESTMAP)

> 本文档为 SubLift 项目的统一测试架构与测试资产总览。
> 它明确了各开发场景下的测试决策路径、全仓测试资产分类与执行方法，以及技术债收敛清单。
> 架构与产品规范参见 `docs/ARCHITECTURE.md` 与 `AGENTS.md`。

---

## 1. 启动基线验证 (Startup Verification)

### 1.1 定位与边界
`./init.sh` 是会话开始或恢复工作前的项目唯一初始化入口（L0 级基线验证）。其职责是确保工作区具备可靠开工的最低条件。

- **执行耗时**：毫秒级响应（< 1s）。
- **执行特性**：严格幂等、零副作用。
- **职责边界**：仅验证 Harness 与进度真源的完整性。**严禁**在 `init.sh` 中引入依赖安装（`uv sync` / `npm install`）、编译构建（`cmake` / `swift build`）、运行测试或交付验收。
- **与质量门禁关系**：`init.sh` 仅建立开工前提；代码正确性与产品交付验收完全由独立的测试命令与门禁脚本（`verify-product.sh` 等）承担。

### 1.2 检查清单
`init.sh` 自动执行以下核心检查：
1. **关键契约与索引文件存在性**：
   - `AGENTS.md`：项目级 Agent 契约与权威规则；
   - `progress.md`：当前开发进度与风险导航；
   - `phases.json`：全生命周期 Phase 真源索引；
   - `docs/TESTMAP.md`：全仓测试地图与运行决策规范。
2. **进度索引语法与关键依赖校验**：
   - 验证 `phases.json` 的 JSON 语法有效性；
   - 遍历 `phases.json` 中声明的各 Phase `detail_file`，验证文件存在性及 JSON 语法合法性。

---

## 2. 行动决策树 (When to Run What)

根据所修改的代码模块，遵循以下行动决策路径执行对应的编译与测试命令：

```text
修改代码
  │
  ├─► cpp/ (Native Core / Pipeline / Worker / Adapters / Server)
  │     ├─► 构建: cmake -S cpp -B build/cpp -G Ninja -DCMAKE_BUILD_TYPE=Debug -DSUBLIFT_REQUIRE_OPENCV=ON -DSUBLIFT_ENABLE_VISION=ON && cmake --build build/cpp
  │     ├─► 快速验证: ./build/cpp/bin/sublift_tests "[tag]" (如 [image], [pipeline], [paddle], [worker], [server])
  │     └─► 完整测试: ctest --test-dir build/cpp --output-on-failure
  │
  ├─► apps/macos/ (SwiftUI 客户端 / ViewModel / IPC Client)
  │     ├─► 构建/运行: cd apps/macos && swift build && swift run SubLiftMac
  │     └─► 运行测试: cd apps/macos && swift test
  │
  ├─► apps/web/ (Vue 3 前端 / Pinia Stores / Components / Utils)
  │     ├─► 单元与无障碍测试: npm --prefix apps/web test
  │     └─► 类型与生产构建: npm --prefix apps/web run build
  │
  ├─► Web Server & API 集成 (cpp/src/server/, scripts/test_server_e2e.py)
  │     ├─► C++ 契约测试: ./build/cpp/bin/sublift_tests "[server]"
  │     ├─► E2E 接口测试: uv run python scripts/test_server_e2e.py
  │     └─► Web Server 质量门: ./scripts/verify-web-server.sh
  │
  ├─► Python 隔离工具 (src/sublift/, src/sublift_offline/, scripts/diagnostics/)
  │     ├─► 依赖同步: uv sync --extra oracle --extra vision --extra paddle
  │     ├─► Lint & 类型检查: uv run ruff check src tests && uv run mypy src tests (全仓: uv run ruff check .)
  │     ├─► Pytest 单元测试: uv run pytest -m "not integration" --no-cov
  │     └─► 离线工具独立门禁: ./scripts/verify-offline.sh
  │
  └─► 提交 / 合并前全量验证
        ├─► 产品主门禁 (Python-Free): ./scripts/verify-product.sh
        └─► 过渡全仓门 (含 Python Oracle): ./scripts/verify-standard.sh
```

### 2.1 模块验证详表

| 修改范围 | 快速局部验证 | 完整模块验证 | 覆盖说明 |
|---|---|---|---|
| **Native C++ 核心与算法** (`cpp/src/core/`, `cpp/src/pipeline/`) | `./build/cpp/bin/sublift_tests "[image]"` / `"[timeline]"` / `"[dedupe]"` / `"[line_select]"` / `"[pipeline]"` | `ctest --test-dir build/cpp --output-on-failure` | 验证内存图像、时间线聚类、行选择与跨帧文本共识算法 |
| **Native Adapters & 引擎** (`cpp/src/adapters/`, `cpp/src/resources/`) | `./build/cpp/bin/sublift_tests "[paddle]"` / `"[vision]"` / `"[models]"` / `"[native_resources]"` | `ctest --test-dir build/cpp --output-on-failure` | 验证 Paddle/ORT、Apple Vision OCR 适配器、模型定位与几何变换 |
| **Native Worker & IPC** (`cpp/src/worker/`, `cpp/src/application/`) | `./build/cpp/bin/sublift_tests "[worker]"` | `ctest --test-dir build/cpp --output-on-failure` | 验证 UDS 协议帧编解码、Path 模式与任务取消契约 |
| **Native Server** (`cpp/src/server/`) | `./build/cpp/bin/sublift_tests "[server]"` | `./scripts/verify-web-server.sh` | 验证 HTTP/SSE 路由、媒体沙箱根、任务队列与 SRT 导出 |
| **macOS 客户端** (`apps/macos/`) | `swift test --package-path apps/macos --filter <TestCase>` | `cd apps/macos && swift test` | 验证 SwiftUI 状态管理、批量任务中心、工作台交互与 IPC 通信 |
| **Web 前端** (`apps/web/`) | `npx --prefix apps/web vitest run <file>` | `npm --prefix apps/web test && npm --prefix apps/web run build` | 验证 Pinia Stores、ROI 遮罩、实时字幕、A11y 与静态构建 |
| **离线工具与基准** (`src/sublift/`, `src/sublift_offline/`) | `uv run pytest tests/<test_file>.py` | `./scripts/verify-offline.sh` | 验证隔离离线命名空间、Benchmark 评分与 parity 门禁包装；冻结 Oracle 算法单测已移除，产品语义由 C++ `[parity]` 锁定 |

---

## 3. 全仓测试与脚本资产总览 (Test & Script Asset Catalog)

SubLift 维护全栈统一的测试资产。资产按生命周期状态分为五类：
- `Active Gate`：当前主干必须通过的活跃质量门禁与单元测试；
- `Core E2E`：覆盖系统端到端链路与集成契约的核心用例；
- `Frozen Parity`：用于保障 Native C++ 与历史 Oracle 行为 100% 一致的冻结黄金集比对；
- `Milestone Challenge`：针对特定开发阶段/实证挑战的验证资产；
- `Diagnostics`：用于性能分析、算法调试与超参数扫描的离线诊断工具。

### 3.1 质量门禁与测试套件总览

| 资产路径 / 目录 | 框架 / 语言 | 生命周期状态 | 目标范围 | 执行命令 |
|---|---|---|---|---|
| `scripts/verify-product.sh`<br>`scripts/verification/verify-product.sh` | Bash | **Active Gate** | 产品主门禁（Python-Free）：C++ Debug 构建、CTest、Swift 测试、Web Vitest、Web Build、CLI 提取及 Web Server HTTP/SSE/SRT 导出 | `./scripts/verify-product.sh` |
| `scripts/verify-offline.sh`<br>`scripts/verification/verify-offline.sh` | Bash | **Active Gate** | 离线工具门禁：uv extras 同步、命名空间隔离、Ruff、Mypy、Pytest 单元测试与无归档生成检查 | `./scripts/verify-offline.sh` |
| `scripts/verify-standard.sh` | Bash | **Active Gate** | 过渡全仓综合门禁：涵盖 Python 静态检查、C++ 构建、Pytest、Cutover Parity 与 Web E2E | `./scripts/verify-standard.sh` |
| `scripts/verify-web-server.sh` | Bash | **Active Gate** | Web Native Server 本地验收门禁：Server 构建、静态前端构建、Server Catch2 测试、Python E2E 与 Vitest | `./scripts/verify-web-server.sh` |
| `cpp/tests/` | Catch2 v3<br>(C++20 / ObjC++) | **Active Gate** | Native C++ 全量单元测试（Core / Pipeline / Adapters / Worker / Server / CLI）与 Parity 黄金集比对 | `ctest --test-dir build/cpp --output-on-failure` |
| `apps/macos/Tests/SubLiftMacTests/` | XCTest<br>(Swift 5.9+) | **Active Gate** | macOS 客户端全量测试（批量任务队列、调度器、工作台状态、无障碍、IPC 客户端等 39 个测试套件） | `cd apps/macos && swift test` |
| `apps/web/src/**/*.test.ts` | Vitest<br>(TypeScript / Vue 3) | **Active Gate** | Web 前端全量测试（Pinia Stores、ROI 映射、时间码、SRT 格式化、无障碍、批量任务等 21 个测试文件） | `npm --prefix apps/web test` |
| `scripts/test_server_e2e.py` | Python 3.12+<br>(urllib / requests) | **Core E2E** | Web Native Server 核心接口端到端回归套件（HTTP/SSE、沙箱安全、Mock/Paddle 提取比对） | `uv run python scripts/test_server_e2e.py` |
| `tests/` | Pytest<br>(Python 3.12+) | **Active Gate** | 隔离离线工具、Benchmark 与 parity 门禁包装（12 个测试文件）。冻结 Oracle 算法单测已移除，不在 pytest 中为退役产品 Python 背书 | `uv run pytest -m "not integration" --no-cov` |

### 3.2 对齐与黄金集资产 (Frozen Parity)

| 资产路径 / 目录 | 框架 / 语言 | 生命周期状态 | 目标范围 | 执行命令 |
|---|---|---|---|---|
| `cpp/tests/parity/` | Catch2 v3<br>(C++20) | **Frozen Parity** | C++ 原生 Parity 测试（比对 Config, Changepoint, Dedupe, Extractor, LineSelect, Paddle, Pipeline, Signature, Timeline, Vision 黄金输出） | `./build/cpp/bin/sublift_tests "[parity]"` |
| `scripts/parity/check_cutover_gate.py` | Python 3.12+ | **Frozen Parity** | C++ Worker 与 Python Oracle 全流水线切量对比门禁（支持 parity、runtime 与 GT 比对） | `uv run python scripts/parity/check_cutover_gate.py --check` |
| `scripts/parity/check_paddle_*.py` | Python 3.12+ | **Frozen Parity** | Paddle OCR 适配切量与性能门禁（`check_paddle_cutover.py`, `check_paddle_gate.py`, `check_paddle_perf.py`） | `uv run python scripts/parity/check_paddle_gate.py` |
| `scripts/parity/dump_*.py` | Python 3.12+ | **Frozen Parity** | 11 个 Oracle 特征与中间结果转储工具（用于生成/更新 changepoint, config, dedupe, extractor, pipeline 等 goldens） | `uv run python scripts/parity/dump_<module>.py` |
| `scripts/parity/gen_*.py` | Python 3.12+ | **Frozen Parity** | 固定测试夹具生成脚本（生成 changepoint, paddle, paddle_quality, signature 测试用例与图例） | `uv run python scripts/parity/gen_<fixture>.py` |
| `scripts/parity/golden_registry.py` | Python 3.12+ | **Frozen Parity** | Parity 黄金集元数据注册表与校验工具 | `uv run python scripts/parity/golden_registry.py` |
| `benchmark/parity/fixtures/`<br>`benchmark/parity/goldens/` | JSON / RGB / PNG | **Frozen Parity** | 版本化固化的算法场景输入与标准黄金比对数据 | 由 CTest `[parity]` 自动加载 |

### 3.3 里程碑挑战与实证测试 (Milestone Challenge)

| 资产路径 / 目录 | 框架 / 语言 | 生命周期状态 | 目标范围 | 执行命令 |
|---|---|---|---|---|
| `scripts/challenge_m2_e2e.py` | Python 3.12+ | **Milestone Challenge** | Phase M2 端到端流水线挑战用例 | `uv run python scripts/challenge_m2_e2e.py` |
| `scripts/challenge_m2_persistence.py` | Python 3.12+ | **Milestone Challenge** | Phase M2 状态与结果持久化挑战用例 | `uv run python scripts/challenge_m2_persistence.py` |
| `scripts/challenge_m3_config.py` | Python 3.12+ | **Milestone Challenge** | Phase M3 配置流转与校验挑战用例 | `uv run python scripts/challenge_m3_config.py` |
| `scripts/empirical_challenge_m4.py` | Python 3.12+ | **Milestone Challenge** | Phase M4 实证性能与识别质量挑战用例 | `uv run python scripts/empirical_challenge_m4.py` |
| `apps/web/src/**/empirical_challenge_*.test.ts` | Vitest (TS) | **Milestone Challenge** | Phase M2/M3/M5/M6 Web Store、View 与组件状态实证挑战测试（含 `components/`, `stores/`, `views/` 7 个文件） | `npx --prefix apps/web vitest run apps/web/src/components/empirical_challenge_m6.test.ts` |

### 3.4 诊断工具与基准资产 (Diagnostics & Benchmarks)

| 资产路径 / 目录 | 框架 / 语言 | 生命周期状态 | 目标范围 | 执行命令 |
|---|---|---|---|---|
| `scripts/diagnostics/compare_detectors.py` | Python 3.12+ | **Diagnostics** | 文本检测器效果与耗时对比诊断 | `uv run python scripts/diagnostics/compare_detectors.py` |
| `scripts/diagnostics/ocr_compare.py` | Python 3.12+ | **Diagnostics** | Paddle 与 Apple Vision OCR 识别精度与性能对比 | `uv run python scripts/diagnostics/ocr_compare.py` |
| `scripts/diagnostics/run_timeline.py` | Python 3.12+ | **Diagnostics** | 时间线切分与字幕段生命周期可视化诊断 | `uv run python scripts/diagnostics/run_timeline.py` |
| `scripts/diagnostics/run_trace.py` | Python 3.12+ | **Diagnostics** | 完整流水线执行时序与 Trace 分析 | `uv run python scripts/diagnostics/run_trace.py` |
| `scripts/diagnostics/scan_params.py` | Python 3.12+ | **Diagnostics** | 变化检测与 OCR 超参数网格搜索 | `uv run python scripts/diagnostics/scan_params.py` |
| `scripts/diagnostics/audit_python_ipc.py` | Python 3.12+ | **Diagnostics** | Python IPC 协议通信与消息交互审计 | `uv run python scripts/diagnostics/audit_python_ipc.py` |
| `scripts/diagnostics/extract_frames.py` | Python 3.12+ | **Diagnostics** | 抽帧诊断与代表帧图像导出 | `uv run python scripts/diagnostics/extract_frames.py` |
| `scripts/diagnostics/long_video_ux.py` | Python 3.12+ | **Diagnostics** | 长视频提取交互体验与进度流转诊断 | `uv run python scripts/diagnostics/long_video_ux.py` |
| `scripts/compare_roi_ab.py` | Python 3.12+ | **Diagnostics** | ROI 裁剪与全画幅抽帧 A/B 质量比对 | `uv run python scripts/compare_roi_ab.py` |
| `scripts/measure_perf_overhead.py` | Python 3.12+ | **Diagnostics** | 流水线各阶段耗时与系统开销微基准测量 | `uv run python scripts/measure_perf_overhead.py` |
| `scripts/cleanup_local_artifacts.py` | Python 3.12+ | **Diagnostics** | 本地测试产生的临时运行产物清理 | `uv run python scripts/cleanup_local_artifacts.py` |
| `scripts/run_benchmark_manifest.py` | Python 3.12+ | **Diagnostics** | Benchmark 数据集清单批量评测执行器 | `uv run python scripts/run_benchmark_manifest.py` |
| `scripts/capture-08511-evidence.sh` | Bash | **Diagnostics** | Phase 8 UI 交互验收证据捕获辅助脚本 | `./scripts/capture-08511-evidence.sh` |
| `benchmark/configs/`<br>`benchmark/datasets/`<br>`benchmark/baselines/` | JSON / SRT / ASS / MD | **Diagnostics** | 评测数据集、矩阵配置与质量/性能归因基线文档 | 由 Benchmark 运行时消费 |

---

## 4. 待办与技术债收敛 (Pending & Technical Debt)

### 4.1 零资产损耗原则 (Zero Asset Loss Principle)
在项目演进与重构过程中，**严禁**未经评估擅自删除、重命名或静默移除任何既有测试脚本、数据集或 Parity 黄金文件。所有历史资产均有其验证追溯价值。

### 4.2 技术债收敛行动清单

- [ ] **TD-01: 里程碑挑战脚本收敛 (Milestone Challenge Consolidation)**
  - **现状**：`scripts/challenge_m*.py` 及 Web 端的 `empirical_challenge_m*.test.ts` 散落于各目录，属于各阶段遗留的实证验收资产。
  - **收敛路径**：在后续重构轮次中，将具有长期回归价值的用例整合入标准 E2E 套件（如 `test_server_e2e.py`）或标准 Vitest 测试中；无须重复运行的历史用例按归档流程统一管理，保持当前文件只读保留。
- [ ] **TD-02: Parity 门禁原生化演进 (Native Parity Gate Convergence)**
  - **现状**：`scripts/parity/check_cutover_gate.py` 依赖 Python 运行时与历史 Oracle 依赖。
  - **收敛路径**：持续完善 `cpp/tests/parity/` 下的纯 C++ Catch2 黄金集比对用例，逐步使 `./scripts/verify-product.sh` 成为唯一自闭环的产品级验收门禁，降低对 Python 环境的依赖。
- [ ] **TD-03: 诊断脚本命名空间与输出规范化 (Diagnostics Standardization)**
  - **现状**：部分诊断脚本位于 `scripts/` 根目录，部分位于 `scripts/diagnostics/`，输出路径散落。
  - **收敛路径**：将离线诊断工具统一规范至 `sublift_offline` 命令空间或统一的 CLI 工具下，输出产物严格限定在 `debug/benchmark/` 目录中。
- [ ] **TD-04: 测试数据集与黄金文件治理 (Dataset & Golden Governance)**
  - **现状**：部分测试集依赖本地生成的临时视频。
  - **收敛路径**：测试用例一律采用 `ffmpeg lavfi` 合成轻量视频或固定尺寸测试图片，严禁向 Git 仓库提交大体积视频二进制资产；所有 Parity 黄金输出保持只读不可变。
