---
name: develop-phase
description: 完整交付或续接 phases 中已登记的单个 Phase：按依赖顺序调用 develop-feature，全部完成后做 Phase 级 Verification。默认连续推进。不要用于拆 Roadmap、并行多 Feature 或未登记的零散改动。
---

# Develop Phase

主 Agent 顶层编排一个 Phase：一次只跑一个 Feature，循环至可交付。硬边界见 `.agent/rules/phase-development.md`；字段以 `.agent/schemas/phase-development-state.schema.json` 与 validator 为准。Feature 内步骤与 plan/review 委派见 `.agent/skills/develop-feature/SKILL.md`。

```text
running（选 Feature → develop-feature → 更新记录）
  → verifying（Phase Verification）
  → completed | blocked
```

## 状态与 receipt

**路径**：`.agent/state/phases/<phase-id>.json`（Git 忽略）。

**新建时**（`schema_version: 3`）：`phase_id`、`detail_file`、`baseline_ref`（当前 HEAD）、`final_ref: null`、`authorization.execution`（authority: user、scope、evidence）、`status: running`、`current_feature_id: null`、`feature_runs[]`（按 detail 每个 feature：`feature_id`、`dependencies`、`status: pending`、`state_ref: null`、`commit_shas: []`、`last_outcome: null`）、`deferred_blockers: []`、`capability_runs: []`、`verification_results: []`、`iteration`（按 schema 初值）、`blocker: null`、`updated_at`。

**Workflow receipt**：启动预分配 `phase-development`（running）；每次调用 Feature 前预分配 `feature-development`。结束前保持 `running`，结构化停止 → `blocked`，交付完成 → `completed`。

**校验**：进入 `verifying` 前、以及标 `completed` 前：

```bash
python .agent/scripts/validate-orchestration.py phase .agent/state/phases/<phase-id>.json
```

## 1. 启动或恢复

1. 确认用户要完成/续接该 Phase。
2. 读 `phases.json` 与目标 `detail_file`；校验 Feature ID 唯一、依赖存在且无环；不补造缺失 Feature。
3. 已有 Phase state 则续跑；否则按上表新建。
4. 导航需要时：`progress.md` 优先 `phase_state_ref`。

## 2. 选 Feature 并调用

每轮只选一个 runnable，写入 `current_feature_id`：

1. 已是 `active` / 跟踪 `in-progress` 的可恢复项；
2. 否则依赖均已 `completed` 的 `pending` 项；
3. 同等条件保持 detail 文件顺序。

调用 **develop-feature**（下传执行意图与 `phase_state_ref`）。返回后以真实 Feature state 更新 Phase 记录：

| 返回 | 处理 |
|---|---|
| `FEATURE_COMPLETED` | state 为 `completed` 且有合法 commit SHA → 运行记录 `completed`、记 SHA，继续下一项 |
| `FEATURE_BLOCKED` | 记 blocker；工作区干净且另有可隔离 runnable → `deferred_blockers[]` 后继续，否则 Phase `blocked` |

默认连续推进；Ask first / Never / 越权 / 范围变化 / 不会修时再停。延后问题合并少问。有 runnable 且未停时不要中途结束循环。

## 3. Phase Verification

全部 Feature 记录均为 `completed` 后：

1. `status: verifying`；目标 `baseline_ref..HEAD` 与已记录提交集合。
2. `.agent/skills/verify/SKILL.md`，`verification_level: phase`。
3. 跑 phase validator；通过则写 `final_ref`，`status: completed`。
4. 失败且可归属：经 develop-feature 完整返工（含 repair commit），再重跑本步。
5. 无法归属或需改范围：`blocked`，不临时加未登记 Feature。

不另做只改状态的空提交。

## 4. 返回

```text
PHASE_COMPLETED | PHASE_BLOCKED
phase_state_ref: .agent/state/phases/<phase-id>.json
feature_commits:
  - <feature-id>: <sha...>
phase_verification: <latest status>
known_gaps:
  - <non-blocking gap>
next_action: deliver | <最小阻塞动作>
```
