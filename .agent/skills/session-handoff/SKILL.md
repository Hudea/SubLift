---
name: session-handoff
description: 结束会话前的收尾清单：按需更新当前 Deliverable、最小 evidence 与风险，并确认初始化基线可接续。不为纯阅读/讨论或会话 Task 制造长期进度记录。
---

# Session Handoff

结束会话前按序执行：

1. **是否写入**：仅当工作绑定已登记且已启动的 Deliverable，并且导航、状态、风险或
   evidence 确有变化时，才做 2–4。纯阅读、讨论、探索设计、未绑定工作或仅完成会话 Task，
   不为记而写 `progress.md`、Phase detail 或 `phases.json`。
2. **progress.md**（按需）：只刷新当前 Phase / Deliverable、未解决风险和下一接续点；遵守
   `.agent/rules/project-continuity.md` 的防膨胀规则，不粘贴结果链。
3. **Deliverable / Phase 状态**（按需）：把最小可复核证据写入 Deliverable `evidence`，并检查
   `phases.json` 与 detail 文件一致。Deliverable 验收完成后标记为 `done`；全部 Deliverable
   完成且分支验收通过后，Phase 最多进入 `ready-for-merge`。只有合入 `main` 且在 `main` 上
   完成必要验收，Phase 才能置 `done` 并转为只读。
4. **风险**（按需）：已启动 Deliverable 的未解决风险写 `progress.md`；探索性判断留在对话
   或正式设计文档，不创建长期 Task 状态。
5. **可接续**：再跑 `./init.sh`。失败则先恢复初始化基线（见
   `.agent/rules/initialization.md`）。如果认为应加入新的测试基线，参见 initialization rule
   向用户提出建议，不自行扩大门禁。

## 详细交接（可选）

跨会话/跨 agent 且内容复杂时，可另写根目录 `session-handoff.md`（上下文、已完成、下一步、关键路径与命令、风险、验证）；用完可删，不长期堆积。
