# UI 参考资产

`assets/` 中的 8 张 PNG 来自用户提供的外部设计包；复制入库时改为稳定英文文件名，原始内容未编辑。

| 文件 | 语义 | 使用边界 | SHA-256 |
|---|---|---|---|
| [01-welcome-empty.png](assets/01-welcome-empty.png) | Welcome / Empty | Workbench 视觉参考 | `357005a224b70aafe59f7c7e36eaeef6775e033bbebe08f17fb18a25a6b6f001` |
| [02-drag-active.png](assets/02-drag-active.png) | 合法视频拖入激活 | 拖入反馈参考 | `80db13fbd74149d8803e1bef76c217cb8fb4d95657a9be73c3f98534c39197c2` |
| [03-workspace-ready-inspector.png](assets/03-workspace-ready-inspector.png) | Ready + Video Inspector | Workbench 结构参考 | `d2d3a34adfbf048b987ad6be41b402476330bd154121a6234f077f5857120a4f` |
| [04-region-editing.png](assets/04-region-editing.png) | Region Editing | 区域交互参考 | `0b50bc43088bdc360f9f70f8b6517a7899058284db49858f5980f10e97602862` |
| [05-workspace-processing.png](assets/05-workspace-processing.png) | Processing | 进度与只读字幕参考 | `23bbec59c1c9bae0492c5e7007556a370c68d2c52b503d6f9301746116faddc3` |
| [06-workspace-review.png](assets/06-workspace-review.png) | Review | 编辑与导出参考 | `21302ca9505c5b4ff245d48c0bcbde38759a2cb6bd3bd3ed6b4d7cebfefbd48b` |
| [07-settings-recognition.png](assets/07-settings-recognition.png) | Recognition Settings | Settings 视觉参考；图中 Automatic 不构成能力 | `293cdf86647dd3bc84873c5856a57d423200c30d8e266a0fd4f37e86af4e9ac0` |
| [08-task-center-future.png](assets/08-task-center-future.png) | Task Center | 信息层级参考；Whisper/ETA 不构成能力 | `5ec8714ce8aeccf5da2df3906e36e4ed255eb295875b0ddd71653c2983aa9724` |

## 使用规则

- 参考图只用于布局、层级、状态和视觉语言，不要求复刻示例媒体、字幕、文件名或数据。
- 不从图片反推未登记能力；与 Requirements、Architecture 或运行时 capability 冲突时以后者为准。
- 实现验收使用 `evidence/` 中的实际应用截图和测试 fixture，不用参考图冒充完成证据。
- 实现偏离参考时，应优先保证真实能力、原生平台行为、可访问性与安全状态。
