# Verify 阶段（Testing）

仅在 `test_stage: verify` 时读取。产品实现由调用方完成；Testing 默认只运行测试，仅 `posthoc` 可在 verify 阶段补建测试侧文件（mode 以 Plan 为准，不另问用户）。
与 **Verification 能力**（交付 DoD）不同：此处只做自动化 Test Gate / 快照冻结。策略见 `../../rules/testing.md`。

1. 从 `feature_state_ref` 恢复同一 Feature 的有序结果链。
   - `required` 必须存在有效 `RED_CONFIRMED`；`characterization` 必须存在有效 `CHARACTERIZATION_CONFIRMED`。
   - `posthoc`：不要求用户再批 mode；不要求也不得伪造 prepare / RED；须补自动化。
   - `verification-only`：须符合 testing rule 准入；每条相关验收有明确 Verification 去向（交 Verify 做完）。
2. 对 `required` / `characterization` 比较当前目标测试与最新已授权的直接 artifact 集合。列表、角色或指纹变化时停止 Gate，返回 `stage_status: failed`、`outcome: TEST_ARTIFACT_CHANGED`、`test_gate: failed`；先经 diagnose 归因或重新授权后才能继续。
3. `required` 重跑原 RED 目标；`characterization` 重跑基线；`posthoc` 创建或运行 post-hoc 自动化测试；`verification-only` 不运行伪测试，但继续冻结实现快照并记录后续 Verification 去向。
4. 按 `.agent/schemas/change-snapshot.schema.json` 的共享 manifest 算法，冻结本轮产品代码、运行时配置、迁移和公共契约变更，得到候选 `tested_change_fingerprint`；按 `.agent/schemas/test-evidence-snapshot.schema.json` 对当前 `test_artifacts[]` 生成 `tested_evidence_fingerprint`。产品快照和测试证据快照保持独立。
5. 运行受影响测试并按风险扩大回归；必要时运行已有 runtime smoke。为每个失败标记 `impact`。
6. 测试结束后重算完全相同的 manifest。若文件集合或内容变化，返回 `stage_status: blocked`、`outcome: INPUT_BLOCKED`、`test_gate: blocked`，且 `tested_change_fingerprint: null`；先稳定实现目标后再完整执行 `verify`。
7. 所有适用 Gate 条件满足且产品与测试证据快照前后一致时，普通模式返回 `TEST_GATE_PASSED / passed`；`verification-only` 返回 `VERIFICATION_REQUIRED / not-evaluated`。两者都必须携带最终 `tested_change_fingerprint` 和 `tested_evidence_fingerprint`；只有前者生成 Testing `phase_evidence`。

除最终 `TEST_GATE_PASSED` 和 `VERIFICATION_REQUIRED` 外，所有结果的 `tested_change_fingerprint` 和 `tested_evidence_fingerprint` 都为 `null`。产品失败返回 `failed / PRODUCT_FAILURE_CONFIRMED / failed`；环境或 flaky 返回 `blocked`。无关且经基线证明的既有失败可为 `non-blocking`，记录到 `known_gaps`；不能可靠判断关联性时必须 blocking。
