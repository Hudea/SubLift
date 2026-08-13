# 08511 Evidence

## 截图索引

| T# | 描述 | Fixture | 状态 |
|----|------|---------|------|
| T01 | 空态 1280：虚线 drop zone，无 Table，无蓝按钮 | `EMPTY` | 待截图 |
| T02 | 空态 drop 高亮：Accent 虚线 + 浅 fill | `EMPTY` + `SUBLIFT_EVIDENCE_TASKCENTER_DROP=1` | 待截图 |
| T03 | 等待队列：位置列可见（≥1100） | `WAITING`（含 importRootURL） | 待截图 |
| T04 | 运行中：进度 + 输出列 | `RUNNING` | 待截图 |
| T05 | Inspector 卡片：单选 waiting，显示位置/输出/配置 Picker | `WAITING` 单选 | 待截图 |
| T06 | 混合状态 + 窄屏（960）：无位置列，位置折进文件单元格 | `MIXED`，窗口宽度 960 | 待截图 |
| T07 | 输出位置菜单：公共根已选 | `WAITING` + `SUBLIFT_EVIDENCE_ACCENT=1` | 待截图 |

## 验证命令

```bash
cd apps/macos
swift test  # 369 XCTest + 140 Swift Testing 全绿
```

## 注意事项

- T01/T02 未实地点击 NSOpenPanel（无 Computer Use 能力）；Toolbar 入口通过代码审查 + `TaskCenterImportKind` 单测验证。
- T03/T05 的 `importRootURL` 来自 `EvidenceShot.makeFixtureState("WAITING")`，已更新为含 `importRootURL` 的 fixture。
- T07 的输出位置菜单通过 `TaskCenterToolbar.outputLocationMenu` 代码审查验证。
