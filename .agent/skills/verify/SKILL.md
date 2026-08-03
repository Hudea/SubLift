---
name: verify
description: 在 Review 通过后（Feature）或全部 Feature 提交后（Phase），对同一交付快照汇总 Testing/Review 与项目标准检查，判定能否提交或交付。不修代码、不改测试、不改进度、不提交。
---

# Verify

主 Agent 执行最终交付门禁。只出结果；父编排写入 `verification_results[]`。策略见 `.agent/rules/verification.md`；形状见 `assets/verification-result.schema.json`。

与 Testing 区分：**Testing = 行为自动化**；**Verification = DoD/交付矩阵**（可引用 Testing 结果，勿无意义重跑同一测试命令）。

## 1. 输入与前序

```yaml
verification_level: feature | phase
state_ref: ...
target_id: ...
verification_target: { kind, base_ref, head_ref, fingerprint }
```

必读：verification rule 进入条件、result schema、对应 state、AGENTS 标准命令。
Feature：合格 Testing verify + 同快照 `REVIEW_PASSED` + Plan ready。
Phase：各 Feature completed+SHA+Feature 级 `VERIFICATION_PASSED`，提交集合与 target 一致。
缺链 → `VERIFICATION_BLOCKED`。证据引用用 state 数组稳定索引。

## 2. 冻结目标

按 change-snapshot 重算 `verification_target`。Feature：`base_ref` = `implementation_baseline_ref`，指纹对齐 Testing/Review。Phase：对齐 `baseline_ref..HEAD` 与已记录提交。不一致则停。

## 3. 检查矩阵

从 AGENTS DoD/命令、Plan、Testing 的 Verification 去向、风险选检查；类别见 rule。
若 `test_mode == verification-only`：Plan/Testing 列出的去向全部列入**必需**并真实执行，不得用无关检查顶替。
逐项记录命令、状态、证据；占位符/缺基建/缺人工证据 → blocking。
不在本 skill 内修产品。

## 4. Gate 与返回

检查后再算同一 manifest：

| 结果 | 条件 |
|---|---|
| `VERIFICATION_PASSED` | 必需项真实通过、验收有证据、无 blocking、快照稳 |
| `VERIFICATION_FAILED` | 已确认需改产品/交付配置 |
| `VERIFICATION_BLOCKED` | 证据不足或不稳定/快照变了 |

- Feature 通过 → `next_action: ready-for-commit`（父流程写 evidence 并 commit skill）。
- Phase 通过 → `ready-for-phase-delivery`。
不写 state/Git；不 push。
