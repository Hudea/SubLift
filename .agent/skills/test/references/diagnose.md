# Diagnose 阶段

仅在 `test_stage: diagnose` 时读取。

1. 从 `feature_state_ref` 的最新失败结果选择最窄命令，先在不修改文件的情况下复现。
2. 对照基线、Feature Plan、验收条件、`change_scope` 和实际输出，逐项归因并标记 `impact`。
3. `PRODUCT_FAILURE` 不修改产品；`TEST_DEFECT` 可以修复测试、fixture、fake、mock 或 test-only 配置；既有、环境和 flaky 问题不得靠修改测试掩盖。
4. 对 `TEST_ARTIFACT_CHANGED` 区分测试缺陷、合法重构、验收变化与未授权削弱。只有验收映射未变且重新运行最小目标后，才登记新 artifact 集合作为后续 verify 的授权集合。
5. 修复测试侧缺陷后运行最小复现和直接受影响测试，并返回符合 Schema 的结果。

测试缺陷已修复时返回 `stage_status: passed`、`outcome: TEST_DEFECT_FIXED`、`test_gate: not-evaluated`；合法且已重新授权的重构返回 `TEST_ARTIFACT_REAUTHORIZED`；验收变化返回 `PLANNING_REQUIRED`。产品问题返回 `PRODUCT_FAILURE_CONFIRMED`，环境或不确定性返回 blocked outcome。

不得修改产品、需求、架构、依赖、CI 或 Feature Development state。
