---
name: commit-message
description: 根据当前工作区 staged/unstaged/untracked 判断提交边界与原子性，并起草简体中文 conventional commit message。不执行 git commit/push，不处理 rebase/merge 等历史操作。
---

# Commit Message

只做**范围判断 + 起草 message**；真正提交见 `.agent/skills/commit`。

## 1. 收集

```bash
git status
git diff --cached
git diff
git log --oneline -3   # 可选
```

信息不全时做保守判断，并写明缺口。

## 2. 边界与原子性

区分 staged / unstaged / untracked，回答：

- 本次该包含什么？什么应留到下次？
- 是否一个清晰目的？有无顺手修复、无关模块、应拆开的改动？

不原子或不清时：**不要假装可直接提交**；说明原因与拆分建议；仅在需要时给「候选 message」并标注前提。

## 3. 类型、scope、正文

格式：`<type>(<scope>): <description>`

- **type** 只选一个：`feat` `fix` `docs` `style` `refactor` `perf` `test` `build` `ci` `chore` `revert`（释义见 `resources/commit-types.md`）
- **scope**：来自路径/模块，不臆造；实在没有可省略
- **description**：简体中文、动词开头、约 50 字内，概括主目的，禁空话
- 明确破坏性变更用 `type!`，必要时 Footer 写 Breaking Change
- 复杂改动可加 Body 短列表（原因与要点，不流水账）

## 4. 输出

严格用以下结构（示例见 `examples/example-output.md`）：

```text
**当前改动判断**:
（staged/unstaged/untracked 概况；是否适合一次提交）

**提交边界建议**:
（本次应包含什么；若应拆分，怎么拆）

**变更分析**:
（scope、type、是否 Breaking Change 及依据）

**改进建议**:
（原子性、遗漏测试等；无则写「无」）

**建议提交信息**:
（可直接使用的 message；若仅候选须标明）
```

## 纪律

- 全文简体中文；不编造需求、模块或 Issue 号
- 先边界、后文案；不确定就保守
- 不复述本 skill 条文；不执行 `git commit` / `git push`
