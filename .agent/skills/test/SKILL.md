---
name: test
description: 为已规划 Feature 做自动化测试：prepare 建 RED/基线、verify 过 Test Gate、diagnose 归因。可写测试侧；不写产品、不做 Review/Verification/提交。结果交父编排写入 testing_results[]。
---

# Test

主 Agent 执行。只读 Feature state，返回 `assets/test-result.schema.json` 结果；父编排校验并追加 `testing_results[]`。策略见 `.agent/rules/testing.md`；阶段细节见 `references/`。

## 惯例

- 每次只跑一个 `test_stage`：`prepare` | `verify` | `diagnose`。
- 不写 Feature state / progress / Phase；不 commit。
- 无 Tester subagent。

## 1. 输入

```yaml
feature_state_ref: ...
feature_id: ...
feature_plan: docs/plans/features/<id>.md
test_stage: prepare | verify | diagnose
test_mode: required | characterization | posthoc | verification-only  # 须与 ready Plan 一致
change_scope: [...]
mode_authorization: null | { reason, authority: plan|..., evidence }  # 可选审计，不要求用户再批
```

必读：testing rule 模式/准入/Gate、result schema、`feature_state_ref`、Plan 中的 `test_mode` 与验收。
按需：Phase Feature 条目、相关测试结构。
`test_mode` 与 Plan 冲突或实现中私自降级 → `INPUT_BLOCKED` / 回 planning。
命令仍为占位或无测试基建 → blocked。写测试前看清 Git。

## 2. 验收映射

每条验收 → 可观察行为、测试类型、目标测试、最小命令。
`verification-only`：每条相关验收的 **Verification 去向** 必须具体；并核对 rule 准入（客观不能测）。省事理由 → 改回 `required`/`posthoc` 或 `INPUT_BLOCKED`。验收糊/冲突 → `INPUT_BLOCKED`。

## 3. 调度

| mode | 路径 |
|---|---|
| `required`（默认） | prepare（RED）→（实现后）verify |
| `characterization` | prepare（GREEN 基线）→ verify |
| `posthoc` | verify 补自动化（不造 RED；不另问用户） |
| `verification-only` | verify 冻快照 + 列出 Verify 必做项（不造伪测试） |

阶段方法（只读当前阶段一份）：

- `references/prepare.md`
- `references/verify.md`
- `references/diagnose.md`

写入边界与失败分类见 testing rule。

## 4. 返回与交接

- 真实命令、目标测试、直接 artifact 指纹。
- artifact 变化 → `TEST_ARTIFACT_CHANGED`，再 diagnose。
- 最终可进 Review：`TEST_GATE_PASSED/passed` 或 `verification-only` 的 `VERIFICATION_REQUIRED/not-evaluated`，且双指纹非 null；其它结果指纹为 null。
- 父编排：产品失败 → 实现；测试缺陷 → diagnose；环境/证据 → blocked；通过 → Review。
