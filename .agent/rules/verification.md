# Verification 规则

> Verification 策略真源。步骤见 `.agent/skills/verify/SKILL.md`；结果形状见 `assets/verification-result.schema.json`。

## 边界与分界

| 能力 | 问题 |
|---|---|
| Testing | 目标行为与回归（自动化） |
| Review | 测试未覆盖的语义/架构风险 |
| **Verification** | **同一快照上能否交付**（DoD / 标准命令 / 人工证据） |

- **只读交付门禁**：不修产品、不改测试/需求/架构、不写 state/Phase/progress/Git。
- 可跑项目已有命令与低副作用验收；构建产物 ≠ 获得改产品权限。
- 父工作流持久化 `verification_results[]`。主 Agent 执行；无 Verification subagent。

## 级别

| level | 时机 | 通过含义 |
|---|---|---|
| `feature` | Review 通过后 | 可更新跟踪并进入 Commit |
| `phase` | 全部 Feature 已提交后 | Phase 可向用户交付 |

输入：`verification_level`、`state_ref`、`target_id`、`verification_target`（含 baseline 与 change-snapshot 指纹）。

## 进入条件

**Feature**：最新 Testing `verify` 为 `TEST_GATE_PASSED/passed` 或 `verification-only` 的 `VERIFICATION_REQUIRED/not-evaluated`；同快照最新 `REVIEW_PASSED`；Testing/Review/`verification_target` 指纹与 `base_ref`（= `implementation_baseline_ref`）一致；Plan `ready` 且无未决 D3。

**Phase**：所有 Feature `completed` 且有 commit SHA；各有 Feature 级 `VERIFICATION_PASSED`；HEAD/baseline/提交集合与 target 一致；无未提交 Phase 范围产品改动、无未解释 blocked Feature。

否则 → `VERIFICATION_BLOCKED`。

## 检查矩阵

来源：`AGENTS.md` 标准命令与 DoD、Plan、Phase 范围、Testing 的 Verification 去向、改动风险。
类别：`build` `lint` `typecheck` `automated-test` `runtime-smoke` `migration` `manual` `visual` `security` `benchmark` `custom`。

- Feature：受影响范围 + 明确要求的交付检查；**不无理由重复** Testing 刚跑过的同一命令（可引用结果）。
- Phase：全局必需命令 + 跨 Feature 集成。
- **`verification-only`**：Testing 列出的 Verification 去向均为**本轮必需检查**，须真实取得证据（manual/visual/runtime/命令等）；不得用无关 build/lint 顶替后宣称通过。
- 占位符命令、做不到的必需人工步骤、缺基础设施 → blocking，不得记通过。

## 快照与证据

用 change-snapshot；开始前与全部检查后重算；变化 → `SNAPSHOT_MISMATCH` / `VERIFICATION_BLOCKED`。
每项记录：类别、命令/操作、是否必需、状态、证据、未跑原因。禁止把「计划跑/应该过」写成真实证据。manual/visual 须写观察对象、操作、结论。

## 失败与 Gate

失败 kind：`PRODUCT_FAILURE` | `TOOLING_FAILURE` | `ENVIRONMENT_FAILURE` | `EVIDENCE_INCOMPLETE` | `SNAPSHOT_MISMATCH` | `FLAKY_OR_INCONCLUSIVE` | `PREEXISTING_FAILURE`。
与范围重叠、挡必需检查、关联不清 → blocking。

| 结果 | 含义 |
|---|---|
| `VERIFICATION_PASSED` | 必需检查真实通过、验收有证据、无 blocking、快照稳定 |
| `VERIFICATION_FAILED` | 已确认需改产品或交付配置 |
| `VERIFICATION_BLOCKED` | 输入/环境/证据/稳定性不足以下结论 |

不自行 commit/push。
