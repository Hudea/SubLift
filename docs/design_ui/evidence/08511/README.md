# 08511 Evidence

## 截图索引

| T# | 描述 | Fixture | 状态 |
|----|------|---------|------|
| T01 | 空态 1280：虚线 drop zone，无 Table，无蓝按钮 | `EMPTY` | ✓ 已截图 |
| T02 | 空态 drop 高亮：Accent 虚线 + 浅 fill | `EMPTY` + `SUBLIFT_EVIDENCE_TASKCENTER_DROP=1` | ✓ 已截图（暗色模式下高亮微妙） |
| T03 | 等待队列：位置列可见（≥1100） | `WAITING`（含 importRootURL → Zootopia/） | ✓ 已截图 |
| T04 | 运行中：进度 + 输出列 | `RUNNING`（42% + interview_01.srt） | ✓ 已截图 |
| T05 | Inspector 卡片：单选 waiting，显示位置/输出/配置 Picker | `WAITING` 单选 | ✓ 已截图 |
| T06 | 混合状态 + 窄屏（960）：无位置列，位置折进文件单元格 | `MIXED`，SUBLIFT_EVIDENCE_WINDOW_WIDTH=960 | ✓ 已截图 |
| T07 | 输出位置菜单：公共根已选 | `WAITING` + `SUBLIFT_EVIDENCE_ACCENT=1` | ✓ 已截图 |

## 验证命令

```bash
cd apps/macos
swift test  # 369 XCTest + 140 Swift Testing 全绿

# 截图脚本
bash scripts/capture-08511-evidence.sh
```

## 注意事项

- T01/T02 通过 `windowBackgroundColor` 背景修复了 cacheDisplay 全黑问题。
- T02 的 drop 高亮在暗色模式下视觉差异微妙（accent + 0.12 fill + 虚线边框）。
- T06 使用 `SUBLIFT_EVIDENCE_WINDOW_WIDTH=960` 注入窗口宽度，避免 GeometryReader 循环依赖。
- T05/T07 未实地点击 NSOpenPanel（无 Computer Use 能力）；Toolbar 入口通过代码审查 + 单测验证。
