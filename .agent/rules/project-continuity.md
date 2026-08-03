# Project Continuity 规则

> 跨会话保持进度连续。启动 / 结束步骤见 `.agent/skills/session-bootstrap`、`.agent/skills/session-handoff`。

## 路径

| 用途 | 路径 |
|---|---|
| Phase 索引 | `phases.json`（schema：`phases.schema.json`） |
| Phase 细节 | `phases.json` → `detail_file`（如 `docs/phases/phase0.json`） |
| Phase 细节 schema | `docs/phases/phaseN.schema.json`（文件名固定；文档里的 `phaseN` 是模式，不是字面路径） |
| 架构 / Feature 计划 | `docs/plans/architecture/<slug>.md`、`docs/plans/features/<feature-id>.md` |
| 可恢复 state | `.agent/state/phases/<phase-id>.json`、`.agent/state/features/<feature-id>.json`（Git 忽略） |
| 会话导航 | `progress.md` |
| 基线入口 | `./init.sh` |

## 工作节奏

- **一次一个 Feature**：只选一个未完成任务（`not-started` / `in-progress`）。
- **计划 ≠ 实施授权**：未明确启动的 Feature 不改任务状态；Plan `ready` 不等于可实施，更不等于 `done`。
- **可接续**：会话结束时应能再跑通 `./init.sh`；有活动 state 时 state 与仓库一致。
- **条件更新**：仅绑定已启动跟踪任务，且导航 / 状态 / 风险 / 证据确有变化时，才改 `progress.md`、Phase 细节；Phase 状态变了再改 `phases.json`。纯阅读、讨论、探索规划做 handoff 检查，但不为记而写。

## 证据与状态落点

| 内容 | 写哪里 | 不写哪里 |
|---|---|---|
| 最终验证证据 | Phase feature 的 `evidence` | `progress.md` |
| 计划正文 | `docs/plans/**` | `evidence` |
| Testing / Review / Verification 完整结果 | Feature 或 Phase **state** 对应数组 | `progress.md`；除最终 `evidence` 外也不写 Phase 细节 |
| 活动导航 | `progress.md` 至多一条 `phase_state_ref` 或 `feature_state_ref` | 结果链、日志原文 |
| 中间候选证据 | 先留在 state；**Feature 级 Verification 通过后**再写入 Phase `evidence` | 提前写入 Phase |

- 谁写 state / 谁写 `evidence`：由 Feature / Phase Development 编排；Testing、Review、Verify skill 本身不改进度文件。
- 聊天上下文不是证据链；schema 升级按新契约重建，不得静默补字段。

## `progress.md` 防膨胀

导航文件，不是历史档案。目标：下次打开 5 秒内知道在哪。

| 节 | 策略 |
|---|---|
| 当前状态 | 始终更新 |
| 进行中 | 仅当前任务，完成即移出 |
| 当前计划 | 可留一个 `docs/plans/**` 链接，不贴正文 |
| Harness state | 可选一条 state ref；不复制结果链 |
| 近期完成 | ≤5 条；超出压缩为「见对应 phase 细节」 |
| 阻塞 / 风险 | 未解决才保留；已解决立即删 |
| 近期决策 | ≤3 条；旧的迁 `docs/DECISIONS.md` |
| 本会话改动文件 | 可选临时节；会话结束时删 |
| 完成证据 | 不写；只写 Phase `evidence` |

## `init.sh`（防膨胀）

`./init.sh` 是**会话入口 L0 烟雾测试**，不是 CI。

| 要求 | 说明 |
|---|---|
| **快** | 模板默认应在 **数秒内** 结束；跑到几十秒～一分钟即严重偏离初衷 |
| **只做** | 必需文件在不在；关键 JSON 能否 parse；`phases.json` 的 `detail_file` 是否存在 |
| **默认不做** | 安装依赖、全量/子集测试、lint、typecheck、构建、schema 深度交叉校验、网络请求 |
| **加重** | 实例化后若真需要，只在脚本末尾**显式追加**，并在本节记下层级；慢检查优先放 CI 或独立脚本（如 `scripts/ci-smoke.sh`），不要塞进默认 `init.sh` |

层级参考：L0 文件/JSON（模板默认）→ L1 启动冒烟 → L2 最快测试子集 → L3 深校验（单独脚本）。
Session bootstrap/handoff 调用 `./init.sh` 时也按此预期：**红则先修基线**；不要把 init 当成「跑完整测试套件」。
