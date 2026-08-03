# Phase Development 规则

> 单 Phase 连续交付的硬边界。步骤见 `.agent/skills/develop-phase/SKILL.md`；状态形状见 `.agent/schemas/phase-development-state.schema.json`。

## 边界

- **消费已定义范围**：只跑 `phases.json` → `detail_file` 里已有 Feature；不拆 Roadmap、不改 Feature 目标、不私自加未登记任务。
- **一次一个 active Feature**；产品实现走 Feature Development，不在本层内联模拟整条 Feature 链。
- **Phase state 唯一写入者**：`.agent/state/phases/<phase-id>.json`；Feature 结果链仍在 Feature state。
- **有用户执行意图才开干**（明确要求完成/续接该 Phase）；不扩大范围、不隐式 push。
- **提交由下层 Feature Development 经 commit skill 完成**；本层不绕过 commit skill，也不替代 Feature 质量路径。
- **状态真源是 Phase state + 各 Feature state**，不是聊天记忆；字段与校验以 schema / validator 为准。
