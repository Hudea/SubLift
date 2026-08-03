---
name: develop-feature
description: 完整开发或续接 phases 中已登记的单个 Feature（含 Phase 编排下传）。串联规划、测试、实现、审查、验证并经 commit skill 提交；默认连续推进。不要用于 Phase 拆解、并行多个 Feature，或未登记的零散改动。
---

# Develop Feature

主 Agent 编排单 Feature 并写产品代码。硬边界见 `.agent/rules/feature-development.md`；字段与阶段以 `.agent/schemas/feature-development-state.schema.json` 与 validator 为准。

```text
planning → testing-prepare（可跳过）→ implementing
→ testing-verify → reviewing → verifying → committing → completed
```

可 `blocked`（记 `resume_stage`、原因、下一步）；恢复回该点，或明确重规划后回 `planning`。

## 状态与 receipt

**路径**：`.agent/state/features/<feature-id>.json`（Git 忽略）。

**新建时**（`schema_version: 6`）至少包含：`feature_id`、`phase_id` / `phase_detail_file` / `phase_state_ref`（独立 Feature 可为 `null`）、`implementation_baseline_ref`（当前 HEAD，写在任何产品改动前）、`feature_plan: null`、`authorization.execution`（authority / scope / evidence）、`current_stage: planning`、`resume_stage: null`、`test_mode: null`、`change_scope: []`、`decisions: []`、`capability_runs: []`、`testing_results` / `review_results` / `verification_results: []`、`iteration` 三计数为 0、`commit_preparation: null`、`commit: { status: pending, sha/message/receipt_ref: null, history: [] }`、`blocker: null`、`updated_at`。

**每次能力调用**：先追加 `running` 的 `capability_runs[]`，返回后把结果写入对应数组并原子更新 state，再把 receipt 标为 `completed` / `failed` / `blocked`。Commit 在真实 `git commit` 前为 `prepared`。

**进入下一阶段前**跑：

```bash
python .agent/scripts/validate-orchestration.py transition <state> --to <目标阶段>
```

提交前后目标阶段分别为 `committing`、`completed`。

## 1. 启动或恢复

1. 确认执行意图（用户点名该 Feature，或 Phase 下传）。
2. 读 Phase `detail_file` 中的目标 Feature；已有 state 则加载并续 `current_stage` / `resume_stage`，否则按上表新建。
3. 工作区能分清本 Feature / 用户已有 / 无关改动；分不清就停。
4. 需要导航时：`progress.md` 至多一条活动 ref（Phase 下优先 Phase state）。continuity / AGENTS 仅在写进度或边界不清时查。

## 2. Plan / Review 委派

调用 `.agent/skills/plan` 与 `.agent/skills/review` 时：

- **默认** `delegation_policy: required`，分别启动 **planner** / **reviewer** subagent（真实调用引用写入 receipt；无 `invocation_ref` 不得内联后报成功）。
- **仅当**运行时确认没有 subagent 能力时用 `unavailable`，主 Agent 按同一 Brief 内联，`executor.kind: inline-fallback`，不得标成 subagent。
- **本编排路径不要用** `preferred`（留给单独探索性调用）。
- 测试、实现、verify、commit 仍由主 Agent 执行，不强制 subagent。

## 3. 编排循环

子能力细节见各自 skill；此处只定顺序与环回。

1. **Plan** → plan skill（按上节委派）。`ready` 后再往下。Plan 须含 **`test_mode`**（默认 `required`）；`verification-only` 须含不可自动化理由 + 逐条 Verification 去向（见 testing rule）。
2. 将 Plan 的 **`test_mode` 写入 state**（实现中不得静默改成更弱 mode；要改 → 回 planning）。
3. **Prepare**（`required` / `characterization`）→ test skill；`posthoc` / `verification-only` 可跳过 prepare 直接实现（**不**再问用户批 mode）。
4. **实现**：主 Agent 按 Plan 写产品代码；更新 `change_scope` / `decisions[]`。Ask first / Never / 越权则停。
5. **Test verify** → **Review**（按上节委派，**不因 test_mode 减弱**）→ **Feature verify**。
   - 失败：按 outcome 环回（修产品、diagnose、回规划等）。
   - 改产品后须再 test verify 再 review。
   - `verification-only`：Verify 必须做完 Plan/Testing 列出的去向，不能用无关检查顶替。
   - 产品问题在 Verification 暴露：回实现后完整再走测/审/验链。
6. **完成**：Verification 通过后写 evidence、任务 `done`、必要时 `phases.json`；走 commit skill；state → `completed`。默认不 push。

Phase 返工：完整再走测/审/验，经 commit skill 做 `kind: repair`，不改写旧 evidence。

## 4. 返回

```text
FEATURE_COMPLETED | FEATURE_BLOCKED
feature_state_ref: .agent/state/features/<feature-id>.json
current_stage: <stage>
next_action: <最小下一步>
```

默认连续推进，不为进度确认打断。
