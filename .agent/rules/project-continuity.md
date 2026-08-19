# Project Continuity 规则

> 跨会话保持进度连续。启动 / 结束步骤见 `.agent/skills/session-bootstrap`、`.agent/skills/session-handoff`。

## 路径

| 用途 | 路径 |
|---|---|
| Phase 索引 | `phases.json`（schema：`phases.schema.json`） |
| Phase 细节 | `phases.json` → `detail_file`（如 `docs/phases/phase0.json`） |
| Phase 细节 schema | `docs/phases/phaseN.schema.json`（文件名固定；文档里的 `phaseN` 是模式，不是字面路径） |
| 会话导航 | `progress.md` |
| 初始化入口 | `./init.sh`（策略：`.agent/rules/initialization.md`） |
| 提交入口 | `.agent/skills/commit/SKILL.md`（须用户明确同意） |

进度真源是 `phases.json` + 对应 `detail_file` + `progress.md`。  
Plan / Test / Review / Verify 与 Task 编排不持久化为 Harness 状态；**不要**用聊天记忆或
本地 `.agent/state/` 模拟进度事实。若目录被本地工具写入，仍由 `.agent/.gitignore` 忽略，
但不作为项目进度真源。

## 层级语义

- **Phase**：一个完整、可验收的产品能力。单个修复、技术步骤、审核轮次或文档清理不单独
  升格为 Phase。
- **Deliverable**：组成 Phase 的纵向结果。每个新 Phase 包含 2–8 个 Deliverable；每项应能
  独立描述 outcome、acceptance、status 与最小 evidence。
- **Task**：为了完成当前 Deliverable 临时执行的步骤。Task 只进入会话计划，不分配长期 ID，
  不写入 Phase JSON，不在完成后沉积。
- Phase 1–12 保持 legacy 结构与历史 evidence；除明确兼容迁移或事实勘误外不重写。
- legacy 字段中指向已移除 plans/reports 的路径只作来源文字，不是活跃链接；不得为满足历史
  字符串重新创建过程归档目录。当前 Phase 与新文档只能引用仍存在的合同或 evidence。

## 状态与收口

Phase 主生命周期为：

```text
not-started → in-progress → ready-for-merge → done
```

Deliverable 使用 `not-started → in-progress → done`；真实受阻时可标记 `blocked`，但不使用
`ready-for-merge`。

- `blocked` 只记录真实外部阻塞，解除后回到原执行阶段。
- `ready-for-merge` 表示实现、范围内验证和 evidence 已完成，但仍在分支上，允许因 review 或
  合入问题继续修改。
- 只有合入 `main`，并在 `main` 上完成所需验收后，Phase 才能置 `done`。
- `done` Phase 是只读历史里程碑，只允许错别字、失效链接或事实勘误。后续缺陷按同一能力
  或发布目标聚合为新的维护 Phase 及其 2–8 个 Deliverable；单个零散修复不独立升格为 Phase，
  也不得继续扩张旧 Phase。
- v2 Phase 在 `phases.json` 与 detail 文件中的状态必须一致。Phase 1–12 的 legacy 索引遵循
  已确认的历史 ADR：明确跳过或后置的 `blocked` Feature 可以与索引 `done` 共存，不据此恢复
  旧 Phase；未取得证据且仍属于原定交付范围的工作不得伪装为 `done`。

## 工作节奏

- **一次一个 Deliverable**：只推进当前 Phase 中一个已登记、未完成的 Deliverable。
- **Task 即用即弃**：需要拆步骤时使用会话计划；不要把执行清单追加进 Phase 历史。
- **设计 ≠ 实施授权**：讨论或设计完成不自动启动 Deliverable，也不等于 `ready-for-merge`。
- **可接续**：会话结束时应能再跑通 `./init.sh`（边界见 initialization rule）。
- **条件更新**：仅绑定已启动 Deliverable，且导航 / 状态 / 风险 / evidence 确有变化时，才改
  `progress.md`、Phase 细节；Phase 状态变了再改 `phases.json`。纯阅读、讨论、探索设计做
  handoff 检查，但不为记而写。

## 证据落点

| 内容 | 写哪里 | 不写哪里 |
|---|---|---|
| 最终验证证据 | Deliverable 的 `evidence` | `progress.md` |
| outcome / acceptance | Deliverable 对应字段 | `progress.md`、聊天记录 |
| 会话 Task | 当前会话计划 | Phase JSON、`evidence` |
| 活动导航 | `progress.md` | 结果链、日志原文、伪造 state |

- 聊天上下文不是证据链。
- `evidence` 只保存测试命令与结果摘要、commit、产物或截图引用；不保存实施日报和 review 对话。
- 不得把 `feature-list.json` 重新当作进度真源。

## `progress.md` 防膨胀

导航文件，不是历史档案。目标：下次打开 5 秒内知道在哪。

| 节 | 策略 |
|---|---|
| 当前状态 | 始终更新 |
| 进行中 | 仅当前 Phase / Deliverable，完成即移出 |
| 近期完成 | ≤5 条；超出压缩为「见对应 phase 细节」 |
| 阻塞 / 风险 | 未解决才保留；已解决立即删 |
| 近期决策 | ≤3 条；旧的迁 `docs/DECISIONS.md` |
| 本会话改动文件 | 可选临时节；会话结束时删 |
| 完成证据 | 不写；只写 Phase `evidence` |

`./init.sh` 的职责、边界与扩展准入见 `.agent/rules/initialization.md`；本文件不重复。

`phaseN.schema.json` 是新 Phase 的结构合同；当前 `./init.sh` 只检查 JSON 语法与 detail 链，
修改 Phase/schema 时仍须显式运行 Draft 2020-12 校验并把结果写入 Deliverable evidence。
