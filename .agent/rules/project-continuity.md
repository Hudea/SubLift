# Project Continuity 规则

> 跨会话保持进度连续。启动 / 结束步骤见 `.agent/skills/session-bootstrap`、`.agent/skills/session-handoff`。

## 路径

| 用途 | 路径 |
|---|---|
| Phase 索引 | `phases.json`（schema：`phases.schema.json`） |
| Phase 细节 | `phases.json` → `detail_file`（如 `docs/phases/phase0.json`） |
| Phase 细节 schema | `docs/phases/phaseN.schema.json`（文件名固定；文档里的 `phaseN` 是模式，不是字面路径） |
| 架构 / Feature 计划 | `docs/plans/architecture/<slug>.md`、`docs/plans/features/<feature-id>.md` |
| 会话导航 | `progress.md` |
| 初始化入口 | `./init.sh`（策略：`.agent/rules/initialization.md`） |
| 提交入口 | `.agent/skills/commit/SKILL.md`（须用户明确同意） |

进度真源是 `phases.json` + 对应 `detail_file` + `progress.md`。  
Plan / Test / Review / Verify 与 Feature/Phase 编排已从活跃 Harness 移出；**不要**用聊天记忆或本地 `.agent/state/` 模拟已归档状态链。若目录被本地工具写入，仍由 `.agent/.gitignore` 忽略，但不作为项目进度真源。

## 工作节奏

- **一次一个 Feature**：只选一个未完成任务（`not-started` / `in-progress`）。
- **计划 ≠ 实施授权**：未明确启动的 Feature 不改任务状态；Plan `ready` 不等于可实施，更不等于 `done`。
- **可接续**：会话结束时应能再跑通 `./init.sh`（边界见 initialization rule）。
- **条件更新**：仅绑定已启动跟踪任务，且导航 / 状态 / 风险 / 证据确有变化时，才改 `progress.md`、Phase 细节；Phase 状态变了再改 `phases.json`。纯阅读、讨论、探索规划做 handoff 检查，但不为记而写。

## 证据落点

| 内容 | 写哪里 | 不写哪里 |
|---|---|---|
| 最终验证证据 | Phase feature 的 `evidence` | `progress.md` |
| 计划正文 | `docs/plans/**` | `evidence` |
| 活动导航 | `progress.md` | 结果链、日志原文、伪造 state |

- 聊天上下文不是证据链。
- 不得把 `feature-list.json` 重新当作进度真源。

## `progress.md` 防膨胀

导航文件，不是历史档案。目标：下次打开 5 秒内知道在哪。

| 节 | 策略 |
|---|---|
| 当前状态 | 始终更新 |
| 进行中 | 仅当前任务，完成即移出 |
| 当前计划 | 可留一个 `docs/plans/**` 链接，不贴正文 |
| 近期完成 | ≤5 条；超出压缩为「见对应 phase 细节」 |
| 阻塞 / 风险 | 未解决才保留；已解决立即删 |
| 近期决策 | ≤3 条；旧的迁 `docs/DECISIONS.md` |
| 本会话改动文件 | 可选临时节；会话结束时删 |
| 完成证据 | 不写；只写 Phase `evidence` |

`./init.sh` 的职责、边界与扩展准入见 `.agent/rules/initialization.md`；本文件不重复。
