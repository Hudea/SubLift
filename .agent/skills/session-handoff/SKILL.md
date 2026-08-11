---
name: session-handoff
description: 结束会话前的收尾清单，当被告知会话已经接近尾声时，帮助用户做好交接：条件更新进度与任务状态、记录风险、确认初始化基线可接续。不为纯阅读/讨论制造进度记录。
---

# Session Handoff

结束会话前按序执行：

1. **是否写入**：回顾本次会话的任务完成情况，仅当工作绑定已登记且已启动的跟踪任务，且导航/状态/风险/证据确有变化时，才做 2–4。纯阅读、讨论、探索规划、未绑定 Feature → 不为记而写 `progress.md` / Phase / `phases.json`。
2. **progress.md**（按需）：刷新当前状态；理解本次会话完成的任务情况，当前的风险点，预测下一次的任务走向，按照文档模板更新。同时需要遵守 `.agent/rules/project-continuity.md` 防膨胀清理。
3. **任务状态**（按需）：检查当前会话任务状态是否被正确标注，包括检查 phases.json 和 detail_file 的证据和完成状态标注，防止虚假标准任务完成状态。
4. **风险**（按需）：已启动任务的未解决风险写 progress；探索性 Planning 的风险留在计划产物。
5. **可接续**：再跑 `./init.sh`。失败则先恢复初始化基线（见 `.agent/rules/initialization.md`）。如果认为本次会话应当加入新的测试基线，参见 `.agent/rules/initialization.md` 向用户提出建议，并做简明解释。

## 详细交接（可选）

跨会话/跨 agent 且内容复杂时，可另写根目录 `session-handoff.md`（上下文、已完成、下一步、关键路径与命令、风险、验证）；用完可删，不长期堆积。
