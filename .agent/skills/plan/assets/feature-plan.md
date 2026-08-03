---
route: direct | architecture-first
plan_type: feature
feature_id: <PHASE-FID>
status: draft
planning_level: L0 | L1
test_mode: required | characterization | posthoc | verification-only
source: "<Phase 细节文件路径>"
created: YYYY-MM-DD
---

# Feature 计划：<Feature 名称>

## 目标

<要实现的行为与用户价值>

## 范围

- <范围内>

## 非目标

- <本次明确不处理>

## 验收标准

- [ ] <可验证结果>

## 架构依据

- `<架构或模块设计文档>`：<本计划遵守的边界>

## 当前实现与复用模式

| 类别 | 位置 | 应复用的模式 |
|---|---|---|
| 入口 | `<路径>` | <说明> |
| 错误处理 | `<路径>` | <说明> |
| 测试 | `<路径>` | <说明> |

## 文件变更

| 文件 | 动作 | 原因 |
|---|---|---|
| `<路径>` | 新增 / 修改 / 删除 | <原因> |

## 实施步骤

### 1. <步骤名称>

- 位置：`<路径或模块>`
- 行为：<具体改变>
- 依赖：无 / 步骤 N
- 验证：<命令或可观察结果>

## 测试模式

- `test_mode`：与 frontmatter 一致（默认 `required`）。
- 若 `verification-only`：不可自动化的**客观**理由；下表每条验收 → Verification 去向（命令或 manual 步骤）。
- 若 `posthoc`：实现后补建的自动化目标列表。

## 验证方案

```bash
<项目真实验证命令；未知时说明缺口，不要编造>
```

- 正常路径：
- 边界条件：
- 错误路径：
- 回归范围：

## 风险与假设

| 类型 | 内容 | 处理方式 |
|---|---|---|
| 风险 / 假设 | <内容> | <处理方式> |

## 执行权限与停止条件

- 执行 Agent 可决定：<D0/D1>
- 主 Agent 可决定：<D2>
- 用户必须决定：<D3>
- 停止条件：<何时不得继续实现>

## 开放问题

- <未解决问题；没有则写“无”>

## Feature Plan Gate

（L0 可减少步骤数量，不可省略目标/范围/验收/验证。）

- [ ] Feature、范围与验收标准一致
- [ ] 架构引用有效且无隐式架构变更
- [ ] 文件与步骤基于真实仓库
- [ ] 依赖顺序和验证方法明确
- [ ] `test_mode` 合理；verification-only 有客观理由与逐条 Verify 去向
- [ ] D3 决策已解决
