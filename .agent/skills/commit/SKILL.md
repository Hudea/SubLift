---
name: commit
description: 执行 git 提交或推送：先走 commit-message 判断范围并起草 message，再 commit；仅用户明确要求时 push。凡 git commit / git push 必须经本流程，不得直接调用。
---

# Commit

强制入口：`git commit` / `git push` 只经本 skill。是否提交由调用方决定（用户要求，或 Feature/Phase 过门后进入）；**默认只 commit，不 push**。

有 Feature state 时按 capability-run schema 写 receipt：先 completed 的 `commit-message / feature`，再 `prepared` 的 `commit / feature`；真实 commit 成功后回写 SHA 与 completed（平台 PostToolUse 或等价）。无 Feature state 时仍先 commit-message 再提交，不写 state。

## 步骤

1. **起草**：调用 `.agent/skills/commit-message/SKILL.md`。有 Feature state 时记下 `commit_message_run_ref`、`proposed_subject`、`proposed_body`。若结论是不适合直接提交，先整理/拆分，不要硬交。
2. **准备**：对照 `AGENTS.md` Definition of Done（验证是否已跑等）。有 Feature state 时写入 `commit_preparation`（含 `scope_validated`、`atomicity_validated`、verification 引用与拟定 message）。
3. **校验**（有 Feature state）：`prepared` commit receipt 后执行
   `python .agent/scripts/validate-orchestration.py transition <state> --to committing`
   失败则停止。
4. **提交**：用拟定 message 执行 `git commit`。失败不得标 completed。
5. **push**：仅当用户明确要求时执行。

分支命名见 `AGENTS.md`。不要在未要求时直接提交到 `main` / `master`。
