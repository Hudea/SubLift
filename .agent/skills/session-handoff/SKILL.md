---
name: session-handoff
description: 结束会话前的收尾清单：条件更新进度与任务状态、记录风险、确认 L0 基线与活动 state 可接续。不为纯阅读/讨论制造进度记录。
---

# Session Handoff

结束会话前按序执行：

1. **是否写入**：仅当工作绑定已登记且已启动的跟踪任务，且导航/状态/风险/证据确有变化时，才做 2–4。纯阅读、讨论、探索规划、未绑定 Feature → 不为记而写 `progress.md` / Phase / `phases.json`。
2. **progress.md**（按需）：刷新当前状态；活动 harness 至多一条 `phase_state_ref` 或 `feature_state_ref`，不复制 Testing/Review/Verification 结果链。按 `.agent/rules/project-continuity.md` 防膨胀清理。
3. **任务状态**（按需）：Phase 细节中 status 有变才改；影响 Phase 时再改 `phases.json`。
4. **风险**（按需）：已启动任务的未解决风险写 progress；探索性 Planning 的风险留在计划产物。
5. **可接续**：再跑 `./init.sh`（L0）。有活动 state 时确认可读、与 schema 一致，并已含本会话最后相关 Testing/Review/Verification/commit 结果。有意保留的未提交改动写在 blocker 中。

## 详细交接（可选）

跨会话/跨 agent 且内容复杂时，可另写根目录 `session-handoff.md`（上下文、已完成、下一步、关键路径与命令、风险、验证）；用完可删，不长期堆积。
