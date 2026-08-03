# Prepare 阶段

仅在 `test_stage: prepare` 且模式为 `required` 或 `characterization` 时读取。

1. 从 `feature_state_ref` 确认当前 Feature 没有矛盾的未解决结果，并运行修改测试前的相关基线。
2. 映射验收条件到可观察行为、测试类型与目标测试。
3. `required` 为新行为或 Bug 编写 test-first / Regression Test；`characterization` 编写或确认行为保护测试。
4. 运行最小目标，确认新测试被发现；登记目标测试和明确选择的直接测试 artifact，使用 `sha256` 原始字节指纹。
5. 排除测试语法、环境、依赖、fixture、既有失败和 flaky 后，按 Schema 返回结果。

返回：

- 有效 RED：`stage_status: passed`、`outcome: RED_CONFIRMED`、`test_gate: not-evaluated`。
- Characterization GREEN：`passed`、`CHARACTERIZATION_CONFIRMED`、`not-evaluated`。
- 行为已存在：`passed`、`BEHAVIOR_ALREADY_PRESENT`、`not-evaluated`，交回 Planning。
- 测试自身错误：`failed`、`TEST_DEFECT_CONFIRMED`。
- 输入、环境或稳定性无法确认：`blocked`，并使用相应 blocked outcome。

不得修改产品代码，也不得持久化 Feature state；父工作流在收到结果后追加状态链。策略见 `../../rules/testing.md`。
