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
        └─► 若改了离线工具: ./scripts/verify-offline.sh

显式过渡混门（非默认，含历史 cutover）: ./scripts/verify-standard.sh
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
- `Transitional Mixed`：显式过渡混门，不是提交/合并默认入口；
- `Core E2E`：覆盖系统端到端链路与集成契约的核心用例；
- `Frozen Parity`：用于保障 Native C++ 与历史 Oracle 行为 100% 一致的冻结黄金集比对；
- `Milestone Challenge` / `archive-only`：阶段挑战原稿，不进产品门；
- `Diagnostics`：用于性能分析、算法调试与超参数扫描的离线诊断工具。

### 3.1 质量门禁与测试套件总览

| 资产路径 / 目录 | 框架 / 语言 | 生命周期状态 | 目标范围 | 执行命令 |
|---|---|---|---|---|
| `scripts/verify-product.sh`<br>`scripts/verification/verify-product.sh` | Bash | **Active Gate** | 产品主门禁（Python-Free）：C++ Debug 构建、CTest、Swift 测试、Web Vitest、Web Build、CLI 提取及 Web Server HTTP/SSE/SRT 导出 | `./scripts/verify-product.sh` |
| `scripts/verify-offline.sh`<br>`scripts/verification/verify-offline.sh` | Bash | **Active Gate** | 离线工具门禁：uv extras 同步、命名空间隔离、Ruff、Mypy、Pytest 单元测试与无归档生成检查 | `./scripts/verify-offline.sh` |
| `scripts/verify-standard.sh` | Bash | **Transitional Mixed** | 显式过渡混门（非默认）：离线静态检查 + C++ 构建 + pytest + 历史 cutover + Vitest/E2E。不是产品门，也不替代 `verify-offline.sh` | 须显式调用 `./scripts/verify-standard.sh` |
| `scripts/verify-web-server.sh` | Bash | **Active Gate** | Web Native Server 本地验收门禁：Server 构建、静态前端构建、Server Catch2 测试、Python E2E 与 Vitest | `./scripts/verify-web-server.sh` |
| `cpp/tests/` | Catch2 v3<br>(C++20 / ObjC++) | **Active Gate** | Native C++ 全量单元测试（Core / Pipeline / Adapters / Worker / Server / CLI）与 Parity 黄金集比对 | `ctest --test-dir build/cpp --output-on-failure` |
| `apps/macos/Tests/SubLiftMacTests/` | XCTest<br>(Swift 5.9+) | **Active Gate** | macOS 客户端全量测试（批量任务队列、调度器、工作台状态、无障碍、IPC 客户端等 39 个测试套件） | `cd apps/macos && swift test` |
| `apps/web/src/**/*.test.ts` | Vitest<br>(TypeScript / Vue 3) | **Active Gate** | Web 前端全量测试（Pinia Stores、ROI 映射、时间码、SRT 格式化、无障碍、批量任务） | `npm --prefix apps/web test` |
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

工作树已无挑战原稿。独有断言在 `apps/web` 正规 Vitest 与 `scripts/test_server_e2e.py`；原文只在 Git 历史。

### 3.4 诊断工具与基准资产 (Diagnostics & Benchmarks)

低频人工诊断只在 `scripts/diagnostics/`。根目录三个 `compare_roi_ab` / `measure_perf_overhead` / `run_benchmark_manifest` 是 historical shim，不是诊断套件；canonical 入口是 `uv run sublift-benchmark`。产物写到调用方指定目录或 `debug/benchmark/`。

| 资产路径 / 目录 | 框架 / 语言 | 生命周期状态 | 目标范围 | 执行命令 |
|---|---|---|---|---|
| `scripts/diagnostics/compare_detectors.py` | Python 3.12+ | **Diagnostics** | 文本检测器裁剪对照（需显式 `--video --out`） | `uv run python scripts/diagnostics/compare_detectors.py --help` |
| `scripts/diagnostics/ocr_compare.py` | Python 3.12+ | **Diagnostics** | Paddle 与 Apple Vision 逐段人工对照 | `uv run python scripts/diagnostics/ocr_compare.py --help` |
| `scripts/diagnostics/run_timeline.py` | Python 3.12+ | **Diagnostics** | 变化点与时间轴查看 | `uv run python scripts/diagnostics/run_timeline.py --help` |
| `scripts/diagnostics/run_trace.py` | Python 3.12+ | **Diagnostics** | 打轴决策 trace 记录与比较 | `uv run python scripts/diagnostics/run_trace.py --help` |
| `scripts/diagnostics/scan_params.py` | Python 3.12+ | **Diagnostics** | 历史短字幕专项参数扫描 | `uv run python scripts/diagnostics/scan_params.py --help` |
| `scripts/diagnostics/extract_frames.py` | Python 3.12+ | **Diagnostics** | 从指定视频抽取有限帧 | `uv run python scripts/diagnostics/extract_frames.py --help` |
| `scripts/compare_roi_ab.py`<br>`scripts/measure_perf_overhead.py`<br>`scripts/run_benchmark_manifest.py` | Python 3.12+ | **historical shim** | 只转发 `sublift-benchmark compare-roi` / `overhead` / `run` | `uv run sublift-benchmark --help` |
| `scripts/cleanup_local_artifacts.py` | Python 3.12+ | **Diagnostics** | 本机生成物卫生（默认 dry-run） | `uv run python scripts/cleanup_local_artifacts.py` |
| `benchmark/configs/`<br>`benchmark/datasets/`<br>`benchmark/baselines/` | JSON / SRT / ASS / MD | **Diagnostics** | 评测数据集、矩阵配置与质量/性能归因基线 | 由 `sublift-benchmark` 消费 |

---

## 4. 待办与技术债收敛 (Pending & Technical Debt)

### 4.1 零资产损耗原则 (Zero Asset Loss Principle)
在项目演进与重构过程中，**严禁**未经评估擅自删除、重命名或静默移除任何既有测试脚本、数据集或 Parity 黄金文件。所有历史资产均有其验证追溯价值。

### 4.2 技术债收敛行动清单

- [x] **TD-01: 里程碑挑战脚本收敛 (Milestone Challenge Consolidation)**
  - **现状**：独有断言已吸收进 Vitest 正规套件与 `test_server_e2e.py`；挑战原稿已从工作树删除，历史只在 Git。
  - **收敛路径**：已完成。勿再新增 `scripts/challenge_*` 或 `empirical_challenge_*.test.ts`。
- [ ] **TD-02: Parity 门禁原生化演进 (Native Parity Gate Convergence)**
  - **现状**：`scripts/parity/check_cutover_gate.py` 依赖 Python 运行时与历史 Oracle 依赖。
  - **收敛路径**：持续完善 `cpp/tests/parity/` 下的纯 C++ Catch2 黄金集比对用例，逐步使 `./scripts/verify-product.sh` 成为唯一自闭环的产品级验收门禁，降低对 Python 环境的依赖。
- [ ] **TD-03: 诊断脚本命名空间与输出规范化 (Diagnostics Standardization)**
  - **现状**：可跑诊断只在 `scripts/diagnostics/`；根上三个脚本是 historical shim；Python IPC 诊断桩已删除。物理目录与 `sublift_offline` 子命令尚未合并。
  - **收敛路径**：后续若再收，只把仍可跑的诊断挂到 `sublift-offline` / `sublift-benchmark`，产物限定 `debug/benchmark/`；三个 historical shim 保持根路径以免断转发单测。
- [ ] **TD-04: 测试数据集与黄金文件治理 (Dataset & Golden Governance)**
  - **现状**：部分测试集依赖本地生成的临时视频。
  - **收敛路径**：测试用例一律采用 `ffmpeg lavfi` 合成轻量视频或固定尺寸测试图片，严禁向 Git 仓库提交大体积视频二进制资产；所有 Parity 黄金输出保持只读不可变。
