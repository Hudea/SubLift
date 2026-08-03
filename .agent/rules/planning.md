# Planning 规则

> 规划策略真源。步骤见 `.agent/skills/plan/SKILL.md`；方法/模板见 plan 的 `references/`、`assets/`。

## 边界

- 只产出可验收计划，不负责实现、测试、审查、提交。
- 支持 **Architecture**（边界/契约/取舍/迁移）与 **Feature**（已接受架构内的文件级步骤与验证）。
- **不做** Phase/Roadmap 拆解。
- 主 Agent 路由 + Plan Gate + 写盘；**Planner 只读**，不得改 `plan_type`、不得递归委派、不得写产品/Git。

## 路由

- `plan_type`：`architecture` | `feature`（产物类型）。
- `route`：`direct` | `architecture-first`（请求如何推进；后者不是第三种 plan_type）。
- Planner 每次只执行一个明确的 `plan_type`。

命中即停：

1. 用户指定类型 → 用该 `plan_type`；显式 Feature 仍依赖未决架构 → `architecture-first`（除非用户排除架构规划）。
2. 交付 Feature 且依赖未决边界/契约/选型/跨模块约束 → `architecture-first`。
3. 只决定系统边界/API/数据所有权/选型/迁移等 → `direct` + `architecture`。
4. 已登记 Feature 且架构与验收已明确 → `direct` + `feature`。
5. 无法判断且选择有实质差异 → 问用户。

`architecture-first`：先 architecture 过门并解决必要 D3，再 feature。修改量/文件数不能单独决定类型。

Planner 返回码：`NEEDS_ARCHITECTURE` | `ROUTE_MISMATCH` | `UNSUPPORTED_PLAN_TYPE` | `NEEDS_INPUT`。

## 深度

| 级 | 场景 | 要求 |
|---|---|---|
| L0 | 边界清、低风险、局部 Feature | 短计划；仍须目标/范围/验收/步骤/验证 |
| L1 | 多文件或普通跨组件 | 完整 Feature Plan |
| L2 | 架构/高风险/跨模块 | Architecture Plan；必要时再 Feature Plan |

## 决策

| 级 | 含义 | 处理 |
|---|---|---|
| D0 | 唯一惯例 | Planner 直接采用 |
| D1 | 局部可逆 | Planner 选并记假设 |
| D2 | 跨步骤但不破边界 | 主 Agent 决定并记录 |
| D3 | 目标/范围/公共行为/架构/不可逆/安全/显著成本 | 用户决定 |

## 写入

仅：`docs/plans/**`；已启动 Feature 的 `progress.md` 导航/风险；已启动 Feature 可标 `in-progress`。
探索性/未绑定 Feature 的规划不改 Phase 状态。禁止改产品、测试、依赖、构建、Git。

## Plan Gate 与 ready

标 `ready` 须同时：类型匹配；目标/范围/非目标/验收或成功判据明确；结论有仓库证据；依赖与验证路径明确；D3 已解决；Architecture 有权衡与边界；Feature 引用已接受架构且无隐式架构变更；Feature 写明 `test_mode`（默认 required；verification-only 须客观理由+逐条 Verify 去向，禁止省事理由）。

- 首次候选保持 `draft`；未解决 D3 → `needs-decision`（不占质量修订轮次）。
- Gate 失败：退回 Planner 修订，最多 2 轮；仍失败 → `PLAN_GATE_FAILED`，保留 `draft`。
- **`ready` ≠ 用户逐项批准，≠ 自动获得实施权限。**
