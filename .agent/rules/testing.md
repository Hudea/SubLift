# Testing 规则

> Testing 策略真源。步骤见 `.agent/skills/test/SKILL.md`；阶段方法见 `references/`；结果形状见 `assets/test-result.schema.json`。

## 边界与分界

| 能力 | 问题 |
|---|---|
| **Testing** | 行为是否满足验收、有无相关回归？（自动化证据） |
| Review | 冻结实现上测试未揭示的语义/架构等问题 |
| Verification | 交付级 DoD（build/lint/人工等）是否齐 |

- **可写**：测试文件、测试数据、fixture/fake/mock、仅影响测试的配置。
- **不可写**：产品代码、需求/架构/验收、依赖、CI/CD、Git；不写 Feature state（只读 `feature_state_ref`）。
- 父工作流校验结果并追加 `testing_results[]`。聊天上下文不是证据链。
- 主 Agent 执行；无 Tester subagent。

## 模式与阶段

输入至少含：`feature_state_ref`、`feature_id`、`feature_plan`、`test_stage`、`test_mode`、`change_scope`。

| test_mode | 含义 | 选用 |
|---|---|---|
| `required` | 新行为/Bug；须有效 RED 后再绿 | **默认** |
| `characterization` | 重构保行为；先 GREEN 基线 | 行为保持型改动 |
| `posthoc` | 跳过 test-first，实现后仍须补自动化 | Plan 写明即可；**不**再问用户批 mode |
| `verification-only` | 客观不能可靠自动化；不造伪测试 | **例外**；见下「准入」 |

**锁定**：`test_mode` 以 ready Feature Plan 为准写入 state；实现阶段不得静默降级。要改 mode → 回 planning 更新 Plan。

**verification-only 准入**（须同时满足）：

1. 主要验收落在：纯文档/文案、必真人看的 UI/视觉、不可测外部依赖/硬件、或项目约定不自动化的一次性运维；且
2. Plan 为**每条**相关验收写明具体 Verification 去向（命令或 manual 步骤）；且
3. 写明**为何不能**自动化（客观限制，非省事）。

**禁止**当作 VO 理由：时间紧、懒得写测、有 Review/Verify 就行、以后再补、可测行为却不测。

`posthoc` / `verification-only` **不**要求用户对 mode 再授权；有 Feature/Phase 执行意图即可。可选在结果里用 `mode_authorization` 记录「依据 Plan §…」（`authority: plan`），便于审计，不是打断点。

`test_stage`：`prepare` | `verify` | `diagnose`。`verify`/`diagnose` 须从 state 恢复前序；链断 → `INPUT_BLOCKED`。

## TDD 与质量

- 按行为选测试类型；不为凑类型造低价值测试。smoke 不能替代行为验收。
- 新行为默认 test-first；Bug 先可复现回归；重构不人造 RED。
- 有效 RED 须运行器真实失败，且对应目标行为（排除语法/环境/既有失败）。
- 不得删断言/skip/放宽期望造 GREEN；`posthoc` 不宣称 RED；`verification-only` 不伪造自动化。
- `prepare` 与 `posthoc` 的 `verify` 可写测试侧；`diagnose` 仅确认 `TEST_DEFECT` 后可改测试。

## Artifact 与快照

- 只登记**直接**选中的 test/fixture/fake/mock/test-config；指纹算法见 `test-evidence-snapshot` schema。
- 最终可进 Review 的 `verify` 须有稳定 `tested_change_fingerprint`（产品侧 change-snapshot）与 `tested_evidence_fingerprint`。
- 列表/角色/内容变化 → `TEST_ARTIFACT_CHANGED`（审计信号，先 diagnose）；实现在测试前后变化 → 不得过 Test Gate。

## 失败与 Gate

失败 kind：`PRODUCT_FAILURE` | `TEST_DEFECT` | `TEST_ARTIFACT_CHANGED` | `PREEXISTING_FAILURE` | `ENVIRONMENT_FAILURE` | `FLAKY_OR_INCONCLUSIVE`。
带 `impact: blocking | non-blocking`；与目标/验收重叠或挡必要测试 → blocking。仅 blocking 决定 stage 与 Test Gate。

三字段互不替代：`stage_status`（本次调用）、`outcome`（业务结果）、`test_gate`（最终自动化门）。

进入 Review 须 `test_stage == verify` 且：

```text
(TEST_GATE_PASSED && test_gate == passed)
|| (verification-only && VERIFICATION_REQUIRED && test_gate == not-evaluated)
```

并带稳定双指纹。`VERIFICATION_REQUIRED` 可进 Review，**不**表示行为已自动化证明；其余验收由 Verification **必须做完** Plan 中的去向（不得只跑无关 lint 就过）。

**Review 不因 test_mode 减弱**（含 verification-only）。

**Test Gate 通过**：验收有自动化或明确 Verification 去向；模式证据完整；artifact 审计过；必要测试与匹配回归过；无 blocking failure / 无法解释 skip/flaky；证据来自真实运行。
