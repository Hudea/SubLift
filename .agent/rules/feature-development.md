# Feature Development 规则

> 单 Feature 交付的硬边界。步骤见 `.agent/skills/develop-feature/SKILL.md`；状态形状见 `.agent/schemas/feature-development-state.schema.json`。

## 边界

- **一次一个 Feature**；不拆 Phase、不并行多个 Feature。
- **主 Agent 写产品代码**；Planner / Reviewer 只读；Testing 可改测试侧，不改产品。
- **结果链写入 Feature state**（`testing_results` / `review_results` / `verification_results` / `capability_runs`）；专职 skill 不直接改 Phase `evidence` 或 `progress.md`。
- **有用户或 Phase 的执行意图才开干**（用户要求做该 Feature，或 Phase 编排下传）；不扩大范围到未授权目标。
- **过质量路径后的提交走 commit skill**；不绕过 skill 直接 `git commit` / `git push`；**不隐式 push**。
- **状态真源**是 `.agent/state/features/<feature-id>.json`，不是聊天记忆；阶段与字段以 schema / validator 为准。
