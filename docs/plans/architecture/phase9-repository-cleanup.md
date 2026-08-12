---
route: architecture-first
plan_type: architecture
status: done
planning_level: L2
source: "用户请求：提交文档/Harness 清理后，开始规划 Phase 9 后续仓库清理优化"
created: 2026-08-10
tracking: "原 Phase 9 批次已完成；09001–09006 现归档于 docs/phases/phase7.json"
---

# 架构计划：Phase 9 仓库清理与可恢复性

> 历史归档说明（2026-08-12）：本文记录原 Phase 9 批次的计划与当时命名；该批次已按
> ADR-0036 吸收到 Phase 7 项目辅助架构，Feature `09001–09006` 的当前真源为
> [`docs/phases/phase7.json`](../../phases/phase7.json)。`phase9` 与
> `docs/phases/phase9.json` 已释放，可用于未来新的 Phase 9。

## 目标

在不丢失历史证据、不破坏 benchmark 可复现性、也不误删本地测试媒体或未合入分支的前提下，降低 SubLift 的仓库认知负担和本机占用。清理完成后，开发者应能快速区分：

1. 当前受支持的产品、验证与 benchmark 入口；
2. 为历史复现暂时保留的兼容面；
3. 可安全重建的构建、缓存和生成产物；
4. 必须由用户明确决定去留的本地媒体、分支和 worktree。

本计划只确定边界、保留策略、迁移顺序和停止条件。用户已于 2026-08-10 确认全部推荐 D3；后续工作直接登记进 Phase 9，一次只启动一个 Feature。

## 范围

- `scripts/verify-standard.sh` 及其仍使用 `SUBLIFT_INIT_*` 的开发者接口。
- `src/sublift/benchmark/`、`benchmark/`、三个历史 benchmark 包装器及其引用。
- `scripts/` 根目录的手工验收、诊断和历史 E2E 脚本。
- 被 Git 忽略的 `.venv/`、`build/`、`apps/macos/.build/`、缓存和 `debug/` 生成产物。
- `.gitignore`、清理说明和必要的 dry-run 工具边界。
- 只读识别旧分支/worktree 风险；默认不纳入删除范围。

## 非目标

- 不修改字幕提取算法、C++/Python runtime 路由、UDS 协议或 SwiftUI 产品行为。
- 不删除 `docs/phases/*.json`、ADR、报告、benchmark fixtures/goldens/baselines 或历史 evidence。
- 不因文件名含 `feat-*`、`legacy`、`fallback` 就判定其可删除。
- 不在未审计等价覆盖前删除手工脚本中的独有验收逻辑。
- 不自动删除本地视频、SRT、模型缓存、分支、远端引用或 worktree。
- 不把清理命令塞回轻量 `./init.sh`。

## 仓库证据快照

盘点日期：2026-08-10；分支：`codex/phase9-docs-cleanup`，基线提交 `08f8516`。

### 本机占用

| 路径 | 约占用 | 性质 | 初步判断 |
|---|---:|---|---|
| `apps/macos/.build/` | 471 MB | SwiftPM 构建产物 | 可重建；清理会增加下次构建时间 |
| `.venv/` | 294 MB | Python 环境 | 可重建；可能需要网络/本地 wheel 缓存 |
| `debug/` | 466 MB | 本地媒体、抽帧和历史报告混合 | 必须先按来源/产物拆分，不能整目录删除 |
| `build/` | 39 MB | C++ 构建产物 | 可重建 |
| `benchmark/` | 748 KB | 62 个 tracked 配置/fixture/golden/baseline 文件 | 体积很小且参与验证，默认保留 |

`debug/` 的主要占用来自 5 个 Zootopia 视频，最大单文件约 215 MB；另有约 31 个抽帧图片及少量报告/SRT。视频是本地输入资产，抽帧和报告通常可重建，两者不能使用同一删除策略。

### 受支持入口与兼容面

| 类别 | 当前事实 | 证据 |
|---|---|---|
| benchmark canonical CLI | `uv run sublift-benchmark` | `pyproject.toml` 的 `sublift-benchmark = sublift.benchmark.cli:main` |
| benchmark 实现 | 14 个 `src/sublift/benchmark/*.py` 模块 | runner/config/matrix/report/diagnostics 等 |
| 历史包装器 | `run_benchmark_manifest.py`、`measure_perf_overhead.py`、`compare_roi_ab.py` | 仅转发 canonical CLI，并打印兼容提示 |
| 历史根标记 | `feature-list.json` fallback | `config.py` 与 `test_benchmark_config.py` 明确覆盖 legacy checkout |
| parity 资产 | `benchmark/parity/fixtures` 与 `goldens` | C++ cutover/parity 测试真源，不是运行垃圾 |
| Phase 专用配置 | `zootopia_feat039_{full,roi}.json` 等 | 仍被版本化报告和质量/性能基线引用 |

三个包装器和 Phase 专用 configs 占用很小；立即删除的收益主要是减少入口数量，代价是历史命令不可直接复现。因此它们属于“兼容策略”问题，而不是磁盘清理问题。

### 根脚本候选

`scripts/` 有 44 个 tracked 文件。`scripts/parity/` 和 `scripts/diagnostics/` 有明确专题边界；根目录还保留下列历史/手工入口：

- 只在历史 evidence 中被引用：`feat040_long_video_ux.py`、`test_cli_e2e.py`、`test_pipeline_e2e.py`；
- 未发现当前文档引用：`audit_memory_push.py`、`extract_frames.py`、`test_cancel.py`、`test_detectors.py`、`test_export_e2e.py`、`test_streaming.py`；
- 兼容包装器：前述三个 benchmark 转发脚本。

“未被文档引用”不等于无价值：这些脚本可能包含真实视频、取消、内存或流式行为的唯一手工验收。实施前必须逐个建立“现有自动测试/正式 CLI 已覆盖”或“先迁移独有检查”的证据。

### Git 对象边界

- `UI/macos-ui-redesign` 仍绑定 `/Users/hudea/Project/SubLift_agy`，worktree 状态为 `prunable`，分支含未合入提交。
- `apps/macos-gui` 相对远端 ahead 5；`benchmark` ahead 1 / behind 1。
- 分支/worktree 删除不可由仓库文件清理顺带执行；它们必须作为独立用户决策处理。

## 保留等级

| 等级 | 定义 | 默认动作 | 例子 |
|---|---|---|---|
| R0 | 当前产品或验证真源 | 保留并明确入口 | `src/`、`cpp/`、`apps/macos/Sources`、`scripts/parity` |
| R1 | 版本化可复现资产/历史证据 | 保留；仅修断链 | goldens、fixtures、baselines、Phase JSON、报告 |
| R2 | 兼容入口 | 有明确 sunset 决策后才移除 | 三个 benchmark wrapper、`feature-list.json` fallback |
| L0 | 本地不可自动重建输入 | 保护；绝不默认删除 | `debug/*.mp4`、`debug/*.mkv`、本地 GT/SRT |
| L1 | 本地可重建结果 | 可列入显式清理 | `debug/frames/`、生成报告、导出 SRT |
| L2 | 环境/构建/缓存 | 可列入显式清理 | `.venv/`、`build/`、`.build/`、工具缓存 |
| G0 | Git 分支/worktree | 本计划默认只读 | UI/benchmark 历史分支和 prunable worktree |

任何文件在无法证明属于 L1/L2 时，按更高保留等级处理。

## 候选方案

### 方案 A：按目录整体清空

- 做法：删除 `debug/`、构建目录、旧脚本、wrapper 和旧分支。
- 优点：磁盘和目录数量下降最快。
- 风险：混删本地源媒体、破坏历史命令、丢失独有手工验收与未合入 Git 工作。
- 结论：拒绝。

### 方案 B：只清缓存，不处理入口

- 做法：只删除 L2 构建/环境目录，保留所有 tracked 文件和 `debug/`。
- 优点：低代码风险，立即释放约 804 MB。
- 风险：入口、脚本和 benchmark 认知负担不变；下次依赖同步和构建成本增加。
- 结论：可作为单独的本机维护动作，但不足以完成仓库整理。

### 方案 C：保留等级 + 分阶段迁移（推荐）

- 做法：先去耦验证命名，再审计脚本等价覆盖，再提供显式 dry-run 的本地产物清理；兼容入口、媒体和 Git 对象分别决策。
- 优点：同时降低认知负担和磁盘占用，不以牺牲复现性换整洁。
- 代价：需要多个独立 Feature，且兼容/删除选择必须先确认。
- 结论：推荐。

## 推荐目标边界

### 1. 验证入口去耦

- 保留 `./init.sh` 的极小职责，不再扩展。
- `scripts/verify-standard.sh` 的环境变量从误导性的 `SUBLIFT_INIT_*` 迁到 `SUBLIFT_VERIFY_*`。
- 直接切换到 `SUBLIFT_VERIFY_*`，不保留旧变量 alias；同步更新 AGENTS、脚本注释与最终提示。
- `/tmp/sublift_init.log` 等临时文件名同步改为 verify 语义，避免重新建立 Harness 关联。

### 2. Benchmark 入口与资产

- `sublift-benchmark` 保持唯一 canonical CLI。
- `benchmark/parity`、datasets、baselines 和仍被报告引用的 Phase 专用 config 按 R1 保留。
- 三个 wrapper 在 Phase 9 内继续作为历史复现 shim，但不作为活跃推荐入口。
- `feature-list.json` fallback 单独处理，不能和 wrapper 删除绑成一次机械清理；移除它会修改产品包的 repo-root 行为和测试契约。

### 3. 手工脚本收口

逐个脚本建立处置表：职责、输入、输出、当前引用、等价自动测试、独有检查、建议动作。

- 有独有验收价值：迁入 `scripts/diagnostics/` 或转化为明确标记的 integration test，并参数化媒体路径。
- 已被自动测试/CLI 完整覆盖：删除脚本，同时保留历史 Phase evidence 原文。
- 只适合一次性取证：Git 历史已保存实现，当前树删除；不新建长期 `archive/` 垃圾目录。
- 不能确认：保留并标记 owner/用途，不猜测删除。

### 4. 本地产物清理

若确有重复执行需求，提供一个默认 dry-run 的受限清理入口；不让 `git clean -fdX` 成为推荐命令。

- 默认只列出候选和预计释放空间。
- L1、Swift build、C++ build、Python cache/venv 使用独立显式 flag。
- 路径必须是仓库内已解析的固定目录；禁止 `$HOME`、`~`、未解析变量和宽泛 glob。
- 永远不匹配 `debug/*.mp4`、`*.mkv`、用户 GT/SRT 或 Git 路径。
- 真正删除前仍需用户明确授权，并报告删除内容与可恢复性。

### 5. 分支/worktree

- Phase 9 仓库文件清理不删除 G0。
- 若用户决定清理，另做只读 ahead/behind、unmerged commit、worktree dirty/prunable 复核，再逐目标授权。

## 迁移切片

以下切片已登记为 `09002`–`09006`，必须一次只启动一个 Feature；计划 `ready` 不等于自动实施全部切片。

1. **09002 验证命名收口**：迁移 `SUBLIFT_INIT_*`、临时日志名和相关文档；验证日常与发布参数矩阵。
2. **09003 脚本处置审计**：生成逐脚本处置表，迁移独有检查，删除有等价覆盖的根脚本。
3. **09004 Benchmark 兼容收口**：保留 wrappers 作为历史 shim；保持 canonical CLI、报告和历史命令的可理解性。
4. **09005 本地产物卫生**：实现 dry-run 清单与分级清理；首次真实删除仍须单独获批。
5. **09006 Phase 9 收尾验证**：运行标准产品门、benchmark/CLI 专项和路径扫描，更新 Phase evidence。

## 验证策略

| 改动 | 必须验证 |
|---|---|
| verify 变量迁移 | `bash -n`；默认/skip/runtime/GT 参数矩阵；AGENTS 与脚本帮助文本一致 |
| 手工脚本迁移/删除 | Ruff、mypy；对应单测/integration test；`rg` 确认非历史活跃引用无断链 |
| benchmark wrapper 处置 | `tests/test_benchmark_*.py`、`test_compare_roi_ab.py`；canonical CLI help 与等价命令 |
| repo-root fallback 变更 | 新 Harness checkout、legacy checkout、无 marker 三类 fixture |
| 本地清理工具 | dry-run 不写盘；路径逃逸/媒体保护/空目录/重复执行测试；真实删除需另获授权 |
| Phase 收尾 | `./init.sh`、`./scripts/verify-standard.sh` 与 JSON/链接/ignored-path 扫描 |

## 回滚与停止条件

- tracked 文件迁移必须按 Feature 原子提交；删除前先确保等价测试或 canonical 命令已存在。
- wrapper 删除造成历史报告无法理解、repo-root 行为回归或标准门失败时，恢复 shim，不以文档措辞掩盖行为断裂。
- 本地清理只处理可重建目录；发现媒体、未知大文件、工作区外路径或 Git 元数据立即停止。
- 发现脚本仍承载独有质量门、性能门、取消/内存稳定性门时，停止删除并先迁移能力。
- 分支/worktree 存在未合入提交时，未获逐目标授权不得删除。

## 风险

| 风险 | 影响 | 缓解 |
|---|---|---|
| 把 tracked benchmark 资产误判为旧产物 | parity/质量门失效 | R1 默认保留；以引用和测试参与度判定 |
| 删除 wrapper 破坏历史命令 | 报告难复现 | D3 决定兼容窗；必要时保留小型 shim |
| 清空 `debug/` 丢失本地媒体 | 真实验收无法重跑 | L0 保护；清理器不匹配媒体扩展名 |
| 清 `.venv`/build 后离线无法恢复 | 开发中断 | dry-run 报告；按目录独立授权；先确认工具链 |
| 一次性脚本含独有检查 | 覆盖下降 | 逐脚本处置表 + 等价测试证据 |
| 顺手删除旧分支/worktree | 未合入工作丢失 | G0 完全隔离，另行审批 |

## 已确认决策（D3）

### D3-1：验证变量兼容窗

- **推荐**：直接改为 `SUBLIFT_VERIFY_*`，旧 `SUBLIFT_INIT_*` 不再保留 alias；当前项目尚未发布稳定自动化接口，继续保留会延长 Harness 耦合。
- 保守：保留一个 Phase 9 周期的 alias 和 warning，再删除。
- **已选**：推荐方案。

### D3-2：三个 benchmark wrapper

- **推荐**：Phase 9 内继续保留为历史复现 shim，但从活跃命令区移到“历史兼容”说明；待确认没有外部使用后再单独删除。三份文件很小，当前删除收益低于复现价值。
- 激进：本轮删除 wrapper，并逐处补当前等价命令。
- **已选**：推荐方案。

### D3-3：本机大目录

- **推荐**：先清 L1 生成帧/报告与 `apps/macos/.build`、`build/`；保留 `.venv/` 和全部本地视频/SRT。执行真实删除前再次展示清单并询问。
- 最大释放：连 `.venv/` 一并重建；仍不自动删除媒体。
- 仅规划：本轮不做任何本地删除。
- **已选**：推荐方案；本次确认只决定策略，不授权立即删除。

### D3-4：分支/worktree

- **推荐**：排除在 Phase 9 文件清理之外，另开一次 Git 历史整理。
- 纳入后续：单独盘点每个分支的独有提交后再逐目标决定。
- **已选**：推荐方案。

## Architecture Gate

- [x] 目标、范围、非目标和成功方向明确
- [x] 结论有仓库文件、引用和磁盘占用证据
- [x] 资产、兼容面、本地产物和 Git 对象边界分离
- [x] 迁移、验证、回滚与停止条件明确
- [x] D3-1 至 D3-4 已由用户确认
- [x] 状态已提升为 `ready`
