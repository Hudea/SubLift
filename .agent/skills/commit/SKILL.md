---
name: commit
description: 执行 git 提交或推送：先走 commit-message 判断范围并起草 message，再 commit；仅用户明确要求时 push。凡 git commit / git push 必须经本流程，不得直接调用。
---

# Commit

强制入口：`git commit` / `git push` 只经本 skill。

## 步骤

1. **准备**：对照 `AGENTS.md` Definition of Done（验证是否已跑等）。检查任务状态、风险、证据、进度等是否已更新。
2. **起草**：调用 `.agent/skills/commit-message/SKILL.md`，起草 commit message。
3. **校验**：确认 message 已经详细描述提交文档范围与内容，且符合 `AGENTS.md` Definition of Done。如果有风险，按照.agent/skills/session-handoff/SKILL.md 记录风险并补充提交，同步记录在 message 中。
4. **提交**：用最终拟定 message 执行 `git commit`。失败不得标 completed。
5. **push**：仅当用户明确要求时执行。

分支命名见 `AGENTS.md`。不要在未要求时直接提交到 `main` / `master`。
