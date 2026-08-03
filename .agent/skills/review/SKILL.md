---
name: review
description: 在 Testing 冻结的同一 Feature 实现快照上做只读语义审查，返回结构化结果。用于 test verify 合格后的首审或修复后的重审。不要用于改代码、跑最终 Verification、改进度或提交。
---

# Review

主 Agent 调度入口条件与落盘；Reviewer 只读审查。策略见 `.agent/rules/review.md`；结果须符合 `assets/review-result.schema.json`。

## 惯例

- **Receipt**：按 capability-run 写 `review` receipt（running → completed/blocked）。结果先经 `python .agent/scripts/validate-result.py review`（或等价），父工作流再追加 `review_results[]` 并完成 receipt。`capability_run_ref` = 本轮 `run_id`。
- **委派**（与 develop-feature 对齐）：默认 `delegation_policy: required` → **reviewer** subagent；仅无 subagent 能力时 `unavailable` + inline-fallback；编排路径不用 `preferred`。
- Review **不写** Feature state / Phase / evidence / progress；只返回结果对象。

## 1. 输入与前提

必读：review rule 进入条件与 Gate、result schema、`feature_state_ref`、Feature Plan、对应 Testing verify 结果。
按需：计划引用的需求/架构/决策、目标 diff 相关代码。

确认：

1. Feature 身份与 state 一致；最新 verify 满足 rule 的合法前序。
2. `review_target.base_ref == implementation_baseline_ref`；`fingerprint` 与 `tested_change_fingerprint` 一致。
3. 按 schema 重算当前 `test_artifacts` 证据指纹，与 `tested_evidence_fingerprint` 一致。
4. `change_scope` / manifest 覆盖本轮产品侧变更（见 rule）。

任一失败 → `REVIEW_BLOCKED`（不可得字段 `null`，不硬审）。

## 2. Brief 与委派

向 Reviewer 提供：Brief、`review_target`、baseline、Testing 结果引用、历史 `review_results[]`（重审时）、`delegation_policy`。
不要让 Reviewer 读本 SKILL.md 当编排说明；Worker 读 rule + schema + 仓库证据。

`review_round` = 已有 `review_results[]` 条数 + 1。

## 3. 审查（Reviewer 执行）

1. **候选遍**：固定七维，理解 diff/调用链/边界，内部列候选（不边读边输出正式 finding）。
2. **验证遍**：逐项回代码/计划/测试证据，补因果与保护缺口；低置信 → uncertainty；nit/无关旧问题丢弃。
3. 重审：按 `finding_key` 填 lifecycle；上轮开放项必须有结局。
4. 结束前再算产品与测试证据指纹；与输入不一致 → `REVIEW_BLOCKED`（需时先再 Testing verify）。

方法细节见 review rule 与 reviewer agent。

## 4. Gate 与返回

按 rule 输出其一：`REVIEW_PASSED` | `CHANGES_REQUIRED` | `REVIEW_BLOCKED`。
结果含：`execution_mode`、`delegation_policy`、`capability_run_ref`、输入/结束两侧指纹、`reviewed_dimensions`、findings、uncertainties、summary、evidence、`next_action`。

父编排：

| 状态 | 下一步 |
|---|---|
| `REVIEW_PASSED` | Feature Verification |
| `CHANGES_REQUIRED` | 修产品 → Testing verify → Review（改产品后禁止跳过 Testing） |
| `REVIEW_BLOCKED` | 补输入/稳快照，或用户决定；命中三轮/无进展则停自动循环 |

控制权交还调用方；不代替实现或 Verification。
