# 示例输出参考

以下是一个标准的场景示例。假设用户工作区中暂存了（staged）一个工具函数的修复，但未暂存（unstaged）包含了几个无关组件的文本调整。

---

**🔍 当前改动判断**:
当前工作区包含两部分内容：
- **Staged**: `utils/dateFormatter.ts` 中修复了跨时区日期解析错误的逻辑。
- **Unstaged**: `components/Header.tsx` 和 `components/Footer.tsx` 中修改了版权文案和 Logo 间距。

这两种改动服务于不同的目的（线上 Bug 修复 vs UI 细节调整），不适合组织成一次提交。

**🧩 提交边界建议**:
建议本次提交**仅包含 Staged 的改动**（处理日期解析的问题）。
Unstaged 的 UI 调整应该保留在工作区，稍后作为一个独立的提交。

**🔍 变更分析**:
- **Scope**: 由于修改发生在 `utils/dateFormatter.ts`，主要影响日期处理逻辑，scope 判定为 `utils`。
- **Type**: 目的为修复线上存在的时区解析 Bug，判定为 `fix`。
- **Breaking Change**: 仅修复内部解析错误，未改变函数签名和预期返回格式，无 Breaking Change。

**⚠️ 改进建议**:
- 原子性：当前的 Staged 内容是高度原子的。
- 遗漏测试：虽然修复了 Bug，但看到同目录下未包含关于 `dateFormatter.test.ts` 的改动。建议补充跨时区的边缘测试用例以防止回归。

**📋 建议提交信息**:

```text
fix(utils): 修复 dateFormatter 跨时区解析导致日期偏移的错误

- 修复了因为未显式设置 timezone 导致在跨时区环境下使用 Date.parse 时偶发的一天偏差
- 将内部解析统一替换为 UTC 标准时间再做本地化格式化转换
```
