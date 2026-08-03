# Review 规则

> Review 策略真源。步骤见 `.agent/skills/review/SKILL.md`；结果形状见 `.agent/skills/review/assets/review-result.schema.json`。

## 边界

三类能力各答一问：

| 能力 | 问题 |
|---|---|
| Testing | 行为是否满足验收、有无相关回归？ |
| **Review** | 冻结实现上是否仍有测试未揭示的真实缺陷/风险/偏离？ |
| Verification | build/lint/交付级证据是否整体成立？ |

- **只读门禁**：不改产品/测试，不跑完整交付验证，不写 Feature state / Phase / evidence / `progress.md`，不 commit/push，不宣布 Feature 完成。
- 父级 Feature Development 校验结果后原子追加 `review_results[]`。
- 主 Agent 调度 + 落 state；**Reviewer 只读**出结构化结果，不判定「该不该进 Review」、不持久化。

## 进入条件

调用方至少提供：`feature_state_ref`、`feature_id`、`feature_plan`、`review_target`（含 `base_ref` = Feature `implementation_baseline_ref`、`fingerprint`）、`change_scope`。

必须从 state 读取**同一 Feature 最新最终 Testing verify** 结果，不得用聊天摘要代替。合法前序：

```text
test_stage == verify
&& (
  (outcome == TEST_GATE_PASSED && test_gate == passed)
  || (test_mode == verification-only && outcome == VERIFICATION_REQUIRED && test_gate == not-evaluated)
)
&& tested_change_fingerprint == review_target.fingerprint
&& review_target.base_ref == implementation_baseline_ref
&& 当前 test_artifacts 证据指纹 == tested_evidence_fingerprint
```

- 普通模式 `TEST_GATE_PASSED`：自动化 Test Gate 已过。
- `verification-only` + `VERIFICATION_REQUIRED`：仅快照冻结，**不**证明目标行为已自动化证明；非自动化验收留给 Verification。

前提不成立、指纹不一致、目标在审查中变化 → `REVIEW_BLOCKED`（尚不可得字段用 `null`，不伪造）。

## 快照

与 Testing 共用 `.agent/schemas/change-snapshot.schema.json` / `test-evidence-snapshot.schema.json`。算法与 manifest 以 schema 为准。

- 产品快照覆盖本轮产品代码、运行时配置、迁移与公共契约；不冒充全仓库。
- 开始与结束都重算产品目标与测试证据；任一变化 → `REVIEW_BLOCKED`。

## 维度与排除

固定七维（V1 不缩小 focus）：`correctness`、`requirements`、`architecture-drift`、`security`、`reliability`、`compatibility`、`testing-gap`。

不报：纯格式/命名偏好、机械 lint、无证据推测重构、与本次变更无关的旧问题、不影响交付的 nit。既有问题仅当本次引入/恶化/暴露为必要依赖、或阻碍本次行为安全时才可进 finding。

## Finding 与重审

正式 finding 须可复现修正：稳定 `finding_key`、位置、触发、影响、与变更因果、证据、保护缺口、修正方向。`confidence < 0.80` → 仅 `uncertainties`。

- `priority`（P0–P3）与 `blocking` **独立**，不机械互推。
- `origin` / 因果枚举见 result schema；因果不清 → uncertainty。
- 重审按 `finding_key` 映射 `new|unchanged|worsened|resolved|not-reproduced`；非 `new` 须 `prior_finding_ref` + lifecycle 证据。上轮开放 finding 不得静默消失。`not-reproduced` 原因不清 → blocking uncertainty。

## Gate 与循环

| 结果 | 含义 |
|---|---|
| `REVIEW_PASSED` | 快照稳定、七维完成、无开放 blocking finding/uncertainty → 只进 Verification |
| `CHANGES_REQUIRED` | 七维完成，存在与变更有因果的开放 blocking finding → 主 Agent 修产品 → Testing verify 新快照 → 再 Review |
| `REVIEW_BLOCKED` | 证据不足、快照变了、blocking uncertainty、或循环无进展 |

父工作流最多自动 **三轮** Review。以下停止自动循环（`REVIEW_BLOCKED` / `stop-review-loop`）：快照未变却要求修改；同一 blocking finding 连续两轮 `unchanged`；两失败形态振荡；三轮仍有 blocking finding。扩大轮次须用户明确决定。
