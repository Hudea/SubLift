---
route: direct
plan_type: architecture
status: ready
planning_level: L2
source: "用户请求：将 SubLift 适配到 /Volumes/lab/pp/my_Harness 的新 Harness 体系"
created: 2026-08-03
---

# 架构计划：SubLift 新 Harness 迁移

## 目标

将 SubLift 的协作编排、进度索引与会话基线接入新 Harness，同时保留已完成产品功能、历史任务证据和现有验证能力。迁移完成后，`phases.json → detail_file → features[]` 是唯一操作性进度入口；新任务可使用 `.agent/` 的 Plan / Test / Review / Verify 流程；日常 `./init.sh` 是秒级 L0，而完整产品验证仍有一个明确、可直接执行的入口。

## 范围

- 从 `/Volumes/lab/pp/my_Harness` 引入 canonical `.agent/` rules、skills、schemas、scripts、agents 与自测；不复制 Finder 元数据，也不删除项目已有 adapter 目录。
- 实例化根 `AGENTS.md`、`phases.json`、`phases.schema.json`、`docs/phases/phaseN.schema.json`、`progress.md`、`README.md` 和 L0 `init.sh`。
- 建立 Phase 索引，登记既有 Phase 1–6 与本次 Phase 7；将历史小数文件名迁为 `phase41.json` / `phase42.json`。
- 用兼容 Phase schema 保持已有 `feat-*` 记录、旧 module/subtask 结构和 archived/completed 历史状态，同时让今后 Harness feature 使用严格的五位 ID、验收标准和规范 subtasks。
- 将旧重型启动门迁入 `scripts/verify-standard.sh`；`init.sh` 默认只进行 Harness L0 检查，兼容旧 `SUBLIFT_INIT_*` 环境变量转发。
- 让 benchmark 的仓库根发现优先以 `phases.json` 为标记，并兼容旧 `feature-list.json`；相应更新测试 fixture。

## 非目标

- 不改变 `sublift` / `sublift-benchmark` CLI、UDS JSON IPC、Python/C++/Swift 产品行为或算法。
- 不重编号、重写或伪造 111 条历史 feature 的验收、审查、测试或 commit 回执。
- 不删除 `feature-list.json`、`.grok/` 工作流、历史文档引用或已有项目 adapter。
- 不修复与 Harness 迁移无直接关联的 release GT 资源路径、分发、签名、公证、模型或质量数据问题。
- 未获用户明确同意，不提交或推送 Git 改动。

## 成功判据

- [ ] 新根 `phases.json` 能索引所有 Phase detail，并成为 `AGENTS.md`、README 和 progress 指向的操作真源。
- [ ] 所有保留的历史 Phase JSON 与新 Phase 7 JSON 均满足项目兼容 schema 的对应分支；`phase41.json` / `phase42.json` 的引用无断链。
- [ ] `.agent/` 的必需规则、skills、schema、validator 与自测完整可用，且 Codex receipt 策略不伪造其他平台 adapter。
- [ ] `./init.sh` 仅执行 L0 并通过；`./scripts/verify-standard.sh` 保留原有 ruff、mypy、C++、pytest 与 cutover parity 日常门。
- [ ] Benchmark manifest 在只有 `phases.json` 的新项目根与只有 `feature-list.json` 的历史项目根均能正确解析。

## 当前架构与证据

| 位置 | 当前职责 / 行为 | 证据 |
|---|---|---|
| `AGENTS.md` | 项目契约将 `feature-list.json` 与重型 `./init.sh` 作为工作流中心 | 启动流程、验证命令与进度规则 |
| `feature-list.json` | 旧项目级 Phase/功能组总览；历史文档和 benchmark 根发现仍引用它 | 根文件与 `src/sublift/benchmark/config.py` |
| `docs/phases/*.json` | 保存 111 条历史 `feat-*` 任务与实际 evidence | `docs/phases/phase1.json`–`phase6.json` |
| `docs/phases/phase41.json` / `phase42.json` | 小数命名且含 `archived` / `completed` 历史状态 | 两份归档 Phase detail |
| `init.sh` | 默认同步依赖、静态检查、C++ build/ctest、pytest 与 parity | 当前 10/10 基线 |
| `src/sublift/benchmark/config.py` | 通过 `pyproject.toml + feature-list.json` 查找仓库根 | `discover_repo_root()` |
| `/Volumes/lab/pp/my_Harness` | 新 Harness 的 canonical `.agent/` 与 L0 / Phase 索引模板 | `AGENTS.md`、`.agent/`、`init.sh` |

## 约束与不变量

- 产品运行时与 Agent Harness 分层：C++ parity/golden harness 仍是产品测试术语，不改名、不混用。
- 既有历史 evidence 是审计记录，不因框架升级编造新的 Feature state、review receipt 或 commit。
- `feature-list.json` 仍可被旧文档和旧 checkout 使用，但不再表示当前开发真源。
- 未来新 Feature 的五位 ID 与严格字段不可被 legacy 兼容分支稀释；兼容只用于已有 `feat-*` 条目。
- 原项目的「未获用户同意不得 commit/push」约束优先于模板的默认提交倾向。
- Harness L0 不能默认安装依赖、跑 lint/test、构建或网络请求；完整日常门必须可从独立脚本重跑。

## 决策问题

- 是否为保留历史而扩展项目 Phase schema，还是重写全部历史记录与文档引用？
- 如何在 L0 与原有完整产品验证之间划分职责而不丢失任何门？
- Codex 尚无模板 adapter 时，如何记录真实子代理执行而不伪造 OpenCode/Claude Code 证明？

## 候选方案

### 方案 A：严格替换所有历史追踪

- 做法：将所有 `feat-*` 改为五位 ID，补齐 acceptance/subtask 字段，重写历史引用并删除 `feature-list.json`。
- 优点：所有 Phase detail 立即完全等同模板 schema。
- 代价与风险：需要改写 111 个历史证据、至少 154 处 `feat-*` 文档引用，并会破坏 benchmark 现有根标记；迁移本身会制造大量不可审计的历史噪声。

### 方案 B：保留旧体系，不引入索引或 L0

- 做法：只复制少量 `.agent/` 文件，继续以 `feature-list.json` 和重型 `init.sh` 工作。
- 优点：改动最少。
- 代价与风险：不满足新 Harness 的索引、会话恢复和 L0 契约，未来任务仍没有一致入口。

### 方案 C：双层兼容迁移（推荐）

- 做法：引入 canonical `.agent/` 与新的 `phases.json`；保留 legacy 记录和 `feature-list.json`，由项目 schema 的 legacy/canonical 分支区分；新 Phase / Feature 只使用严格分支。
- 优点：新工作流从本次起可用，历史证据与文档链接保持稳定，产品根发现有平滑兼容路径。
- 代价与风险：schema 比模板多一层兼容逻辑，需明确其只服务于历史数据并以迁移 ADR 固化。

## 推荐方案

采用方案 C。它在不伪造历史事实的前提下落实新 Harness 的全部运行时入口。现有 `feature-list.json` 在产品代码中仍是根标记，且已有 Phase 文件字段/状态与严格模板不兼容；将它们保留为只读历史兼容面，比机械重写更符合证据完整性和低风险迁移原则。

## 目标边界与契约

- **协作编排**：`.agent/` 是新 Harness 的 canonical 执行协议；state 位于 `.agent/state/` 并被 Git 忽略。Codex 委派只记录实际 `platform: codex`、task/invocation reference，不冒充需要 adapter attestation 的平台。
- **进度**：`phases.json` 只包含 `id/name/description/detail_file/status`，detail 中才保存 `features[]`。Phase 1–6 为历史记录，Phase 7 是本次严格 Harness feature 的首个实例。
- **Phase schema**：legacy branch 接受当前 `feat-*`、string module、历史 subtask 与 `archived/completed`；canonical branch 强制 template 定义的 feature ID、`module[]`、`acceptance_criteria`、规范 subtask 和四态 status。两分支的 ID pattern 不重叠。
- **验证**：`init.sh` 仅验证必需文件、JSON 可读与 index → detail 链。`scripts/verify-standard.sh` 是原每日完整门的唯一迁移位置；AGENTS 明确普通、macOS 和发布级命令。
- **兼容**：benchmark root discovery 接受 `pyproject.toml + phases.json`（首选）或 `pyproject.toml + feature-list.json`（legacy fallback）。

## 迁移与回滚

1. 复制 Harness protocol，实例化项目契约与 schemas；新增 Phase 7，先不更改产品行为。
2. 创建 Phase index，迁移小数 detail 文件名并更新精确文件引用；保留 `feature-list.json` 为 legacy compatibility index。
3. 将重型 init 内容移动到独立标准验证脚本，以新的 L0 init 替代；保留环境变量兼容转发。
4. 修改 benchmark root discovery 与其单测，确保新标记和旧标记都可用。
5. 运行 L0、Harness 自测/validator、聚焦 benchmark 测试和完整标准验证；将实际证据写入 Phase 7。

- 回滚条件：L0 无法恢复、标准门有未解释回归、历史 phase/detail 链断裂，或 benchmark 无法在 legacy marker 下解析。
- 回滚方式：所有迁移都限于文档、Harness 文件、验证脚本与根发现；可按文件恢复旧 `init.sh`、phase 文件名/引用和根标记逻辑，不触及视频处理产品数据或历史 evidence 内容。

## 验证方案

- `./init.sh`：验证新 L0 与 Phase index/detail 链。
- `python3 -m unittest discover -s .agent/tests -v`：验证引入的 Harness validator 行为。
- `uv run pytest tests/test_benchmark_config.py tests/test_benchmark_matrix.py tests/test_roi_output_path.py`：覆盖新/旧仓库根标记。
- `./scripts/verify-standard.sh`：保留并运行原有 ruff、mypy、C++/ctest、非集成 pytest、cutover parity 日常门。
- `jq empty` 覆盖所有新的 JSON/schema/index，外加 `rg` 确认旧小数 phase 文件引用已清零。

## 风险与缓解

| 风险 | 影响 | 缓解方式 |
|---|---|---|
| legacy schema 过宽，未来任务误用旧结构 | 新记录质量下降 | schema 将 legacy 分支锁定为既有 Phase 1–6 的固定 metadata；AGENTS 明确新任务只能走 canonical 分支 |
| L0 替换误丢完整验证 | 回归在会话入口不可见 | 原 init 原样迁移到独立脚本并实际运行 |
| 文档有小数 phase 文件链接 | 历史引用断裂 | 仅替换精确 `phase41.json` / `phase42.json` 路径，验证 `rg` 零命中 |
| Codex 没有 adapter | receipt 可能被误报为其他平台 | 只写真实 platform/call reference，不修改 validator 伪造 attestation |
| 现有 release GT gate 有独立资源路径风险 | 迁移验证被无关问题阻断 | 日常标准门保持原 `--skip-gt`；将 release 资源问题留给独立 Feature |

## 执行权限与停止条件

- Planner 可决定：D0（模板文件复制、路径与命名的机械适配）。
- 主 Agent 可决定：D1/D2（历史兼容 schema、legacy marker fallback、L0/标准门的迁移位置），均在用户明确要求适配新 Harness 的范围内。
- 用户必须决定：D3（删除历史追踪、改变产品 CLI/协议、修改 commit/push 授权、引入第三方依赖或重写 release 策略）。
- 停止条件：出现需删除/重写历史 evidence、破坏产品协议、修改现有 adapter 的情况，或验证需要不可用外部资源且无独立日常门可证明迁移。

## 开放问题

- 无。Codex adapter 的强 runtime attestation 作为未来 Harness 演进项；本次采用透明的非伪造 receipt 约定。

## Architecture Gate

- [x] 成功判据明确且可检查
- [x] 方案权衡完整
- [x] 推荐结论有仓库证据
- [x] 边界与契约明确
- [x] 迁移、回滚与验证路径明确
- [x] D3 决策已解决
