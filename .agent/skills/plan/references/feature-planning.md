# Feature Planning 方法

仅在 `plan_type: feature` 时读取。

## 输入前提

- `feature_id` 在当前 Phase 细节中存在。
- 目标、范围、非目标、验收明确。
- 架构边界已定，`architecture_refs` 有依据。

否则若需重定模块边界/公共契约/数据所有权/选型/跨模块约束 → `NEEDS_ARCHITECTURE`。

## 深度

- **L0**：单模块、低风险、边界清 → 步骤 2–4 个即可；仍须目标/范围/验收/验证。
- **L1**：多文件或跨组件 → 完整步骤与文件表。
- 未决架构 → 不要硬写 Feature Plan，回主 Agent 走 architecture-first。

## 调查顺序

1. Feature 定义、需求、架构引用、相关决策。
2. 实现入口、调用链、数据流、受影响模块。
3. 应复用的命名、错误处理、测试模式。
4. 新增/修改/删除文件及理由；不确定路径标待确认。
5. 按依赖拆可独立验证的步骤（一步一目的）。
6. 每步：行为、验证、失败信号。
7. 正常/边界/错误路径与必要回归。
8. 风险、假设、D3、停止条件。

## 输出

- 结构用 `../assets/feature-plan.md`；frontmatter 含 **`test_mode`**（默认 `required`）。
- 步骤到文件或模块，不写实现代码。
- 验证命令用项目真实命令；未实例化则标明缺口。
- `verification-only` 仅客观不能自动化；写理由 + 逐条 Verification 去向（见 testing rule）。勿用「以后再测」等省事理由。
- 不借 Feature Plan 引入未批准依赖或架构变更。
- Gate 勾选对照模板末节与 planning rule。
