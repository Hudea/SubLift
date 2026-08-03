---
name: plan
description: 为仓库生成基于真实代码与文档的中文 Architecture 或 Feature 计划。用于明确要求规划、Feature 实施前详细计划，或暴露未决架构时。负责路由、委派 Planner、持久化与 Plan Gate。不要用于 Phase/Roadmap 拆解、实现、测试、审查或提交。
---

# Plan

主 Agent 路由并验收；Planner 只读调查并返回 draft。策略见 `.agent/rules/planning.md`；方法/模板见 `references/`、`assets/`。

## 惯例

- **Receipt**：按 capability-run schema 写 `plan` receipt（running → completed/blocked）。
- **委派**（与 develop-feature 对齐）：默认 `delegation_policy: required` → **planner** subagent；仅无 subagent 能力时用 `unavailable` + inline-fallback；编排路径不用 `preferred`。
- **Validator**：仅当本轮绑定已有 Feature state 时，持久化后可跑
  `python .agent/scripts/validate-orchestration.py feature <feature-state>`
  纯架构/未建 Feature state 时不要跑。

## 1. 输入

必读：规划目标相关切片（Phase 中该 Feature，或架构问题陈述）+ planning rule 的路由节。
按需：`AGENTS.md`、`docs/REQUIREMENTS.md`、`docs/ARCHITECTURE.md`、`docs/design/*`、`docs/DECISIONS.md`、相关代码。
证据以仓库为准；未知标未知。

## 2. 路由

按 planning rule 确定 `route` 与当前 `plan_type`。
`architecture-first` 分两拍：先 `architecture` 过门并解决必要 D3，再 `feature`。
Phase/Roadmap → `UNSUPPORTED_PLAN_TYPE`。
深度：L0/L1/L2 见 rule；L0 仍用 Feature 模板，步骤可短（2–4 步），不可省略目标/范围/验收/验证。

## 3. Brief 与委派

```yaml
route: direct | architecture-first
plan_type: architecture | feature
goal: ...
scope: [...]
non_goals: [...]
acceptance_criteria: [...]
architecture_refs: [...]
constraints: [...]
open_questions: [...]
# feature 必填 feature_id；architecture 必填 decision_questions
delegation_policy: required | unavailable   # 编排默认 required
```

交给 Planner：Brief + planning 边界/决策节 + 对应 `references/*` + `assets/*`。
不要让 Planner 读本 SKILL.md。Planner 只返回 **draft** 正文与决策状态，不得标 `ready`。

- Architecture：`references/architecture-planning.md`、`assets/architecture-plan.md`
- Feature：`references/feature-planning.md`、`assets/feature-plan.md`

## 4. 持久化与 Gate

| 类型 | 路径 |
|---|---|
| Architecture | `docs/plans/architecture/<slug>.md` |
| Feature | `docs/plans/features/<feature-id>.md` |

frontmatter 写入 `route`；状态 `draft` →（D3）`needs-decision` → Gate 通过 `ready`。同路径覆盖修订，历史靠 Git。

仅当已登记 Feature **已被用户或上层明确启动** 时，才改 `progress.md` 导航或任务 `in-progress`。探索性规划只写计划文件。

**Gate**（细则见 planning rule）：类型匹配、范围/验收清楚、有仓库证据、步骤可验证、无未决 D3、Feature 无隐式架构变更。
Feature Plan 另须：**写明 `test_mode`**（默认 `required`）。若 `verification-only`：客观不可自动化理由 + **每条**相关验收的 Verification 去向；省事理由不得 ready。`posthoc` 须说明实现后补哪些自动化。**不**把「再问用户批 mode」当作 Gate。
失败则反馈缺口修订，最多 2 轮；否则 `PLAN_GATE_FAILED`。

## 5. 返回

```text
route: direct | architecture-first
plan_type: architecture | feature
status: ready | needs-decision | draft
artifact: docs/plans/...
next_action: ready-for-feature-plan | ready-for-implementation | user-decision-required | manual-review-required | PLAN_GATE_FAILED | PLAN_BLOCKED
```

控制权交还调用方；不自行实现。仅规划请求到此停止；已授权后续阶段时由调用方接续。
