---
name: session-bootstrap
description: 会话开始前的轻量启动清单，帮助用户审查工作区状态，明确会话任务：确认目录、读 AGENTS、跑 init、恢复任务上下文。
---

# Session Bootstrap

开始写代码或改产品前，按顺序执行（保持轻量，避免一上来通读全书）。

1. **工作目录**：`pwd` 应为项目根。
2. **契约**：若本会话尚未读过，读根目录 `AGENTS.md`。
3. **初始化**：运行 `./init.sh`。失败时先恢复初始化基线并报告用户，不要直接进入产品实现。初始化边界见 `.agent/rules/initialization.md`。
4. **进度与状态**（按需恢复，不全文复制结果链）：
   - 阅读 `progress.md`（导航用），了解项目运行状态。
   - 打开 `phases.json` → 当前 phase 的 `detail_file`（如 `docs/phases/phase0.json`）→ 进一步明确当前的任务推进，分析下一步的可能任务。
   - 在用户之后的提示中进一步确定当前会话的核心任务。纯阅读、讨论、探索规划：不强制选任务，不改跟踪文件。
5. **深读（按任务再开，非默认）**：需要实现/规划时再读 `README` 相关节、`docs/ARCHITECTURE.md`、`docs/REQUIREMENTS.md`、相关 `docs/design/*`。
6. **近况**：Git 仓库则 `git log --oneline -5`。
