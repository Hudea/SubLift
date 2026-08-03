---
name: session-bootstrap
description: 会话开始前的轻量启动清单：确认目录、读 AGENTS、跑 L0 init、恢复 phases/state 与任务上下文。绑定已登记 Feature 时再选任务；纯阅读/讨论/探索规划可不绑定。
---

# Session Bootstrap

开始写代码或改产品前，按顺序执行（保持轻量，避免一上来通读全书）。

1. **工作目录**：`pwd` 应为项目根。
2. **契约**：若本会话尚未读过，读根目录 `AGENTS.md`（Always / Ask / Never、标准命令、Harness 导航）。
3. **基线**：运行 `./init.sh`（L0，应数秒内结束）。失败则先修基线，再扩大工作范围。
4. **进度与状态**（按需恢复，不全文复制结果链）：
   - 扫 `progress.md`（导航用）。
   - 打开 `phases.json` → 当前 phase 的 `detail_file`（如 `docs/phases/phase0.json`）。
   - 扫描 `.agent/state/phases/*.json` 与 `.agent/state/features/*.json` 中未完成状态；`progress.md` 的 state ref 须指向同一活动状态。优先恢复 Phase state，再定位 Feature state。
   - 工作对应已登记 Feature 时：只选一个 `not-started` / `in-progress`（见 project-continuity「工作节奏」）。
   - 纯阅读、讨论、探索规划：不强制选任务，不改跟踪文件。
5. **深读（按任务再开，非默认）**：需要实现/规划时再读 `README` 相关节、`docs/ARCHITECTURE.md`、`docs/REQUIREMENTS.md`、相关 `docs/design/*`。
6. **近况**：Git 仓库则 `git log --oneline -5`。

完成 1–4（及按需 5–6）、且 init 通过后，再开始后续工作。
