# 08511 Evidence

七张图均为 2026-08-13 重拍。MD5 互不相同。`contentView` 截图**不含窗口 Toolbar**。

| T# | 必须看见 | 读图结果 |
|---|---|---|
| T01 | 虚线 drop zone、无 Table、无蓝按钮 | 通过。文案「将视频拖到这里」+ 隐私句。 |
| T02 | Accent 高亮，与 T01 不同文件 | 通过。DROP fixture 整区蓝色填充；与 T01 哈希不同。 |
| T03 | 扫描摘要 + 位置列 + sidecar 预览 | 通过。接受 2 / 跳过 0 / 拒绝 1；位置 `sublift-b02-fixture/`；输出 `*.srt`。 |
| T04 | 运行中真实进度 | 通过。提取中 42% + `interview_01.srt`。 |
| T05 | Inspector 卡片 | 通过。无 Task ID；位置 `Zootopia/`；输出 `Zootopia / interview_01.srt`；引擎/质量 `labelsHidden`。 |
| T06 | 960 无位置列 | 通过。列=文件/状态/进度/输出；`Zootopia/` 折进文件单元格。 |
| T07 | 公共根已生效 | 通过。Inspector「输出 SubLiftSRT / interview_01.srt」。菜单本身因 Toolbar 不在 contentView 未拍到。 |

## Fixture

| T# | 环境 |
|---|---|
| T01 | `TASKCENTER=EMPTY` |
| T02 | `EMPTY` + `TASKCENTER_DROP=1` |
| T03 | `EMPTY` + `SCAN=1` |
| T04 | `RUNNING` |
| T05 | `WAITING` + `SELECT=1` |
| T06 | `MIXED` + `COMPACT=1` + `WINDOW_WIDTH=960` |
| T07 | `WAITING` + `SELECT=1` + `OUTPUT_ROOT=/tmp/SubLiftSRT` |

重拍脚本已移除。本目录截图即 08511 验收证据。

## 未执行

- 无 Computer Use：未实地点击 Toolbar / NSOpenPanel / 拖入真文件。
- 无 VoiceOver。
- Toolbar「输出位置」菜单展开态未截到（截图只含 contentView）。
