# 参考资产索引与适用边界

来源：用户提供的外部设计包（未纳入仓库）。2026-08-11 经授权复制并改为稳定英文文件名；原图均为 1448 × 1086 PNG，内容未编辑。

| 文件 | 语义 | 使用范围 | SHA-256 |
|---|---|---|---|
| [01-welcome-empty.png](assets/01-welcome-empty.png) | Welcome / Empty | Phase 10 视觉基线 | `357005a224b70aafe59f7c7e36eaeef6775e033bbebe08f17fb18a25a6b6f001` |
| [02-drag-active.png](assets/02-drag-active.png) | 合法视频拖入激活 | Phase 10 视觉基线 | `80db13fbd74149d8803e1bef76c217cb8fb4d95657a9be73c3f98534c39197c2` |
| [03-workspace-ready-inspector.png](assets/03-workspace-ready-inspector.png) | Ready 工作区、Transcript、Video Inspector | Phase 10 结构基线 | `d2d3a34adfbf048b987ad6be41b402476330bd154121a6234f077f5857120a4f` |
| [04-region-editing.png](assets/04-region-editing.png) | Region Editing + Inspector | Phase 10 交互基线 | `0b50bc43088bdc360f9f70f8b6517a7899058284db49858f5980f10e97602862` |
| [05-workspace-processing.png](assets/05-workspace-processing.png) | Processing、Stop、只读实时字幕 | Phase 10 状态基线；只显示真实数据 | `23bbec59c1c9bae0492c5e7007556a370c68d2c52b503d6f9301746116faddc3` |
| [06-workspace-review.png](assets/06-workspace-review.png) | Review、Subtitle Inspector、Export | Phase 10 状态基线 | `21302ca9505c5b4ff245d48c0bcbde38759a2cb6bd3bd3ed6b4d7cebfefbd48b` |
| [07-settings-recognition.png](assets/07-settings-recognition.png) | Recognition Settings | Phase 10 视觉参考；Automatic 不在当前范围 | `293cdf86647dd3bc84873c5856a57d423200c30d8e266a0fd4f37e86af4e9ac0` |
| [08-task-center-future.png](assets/08-task-center-future.png) | Task Center | Phase 8 信息层级参考；Whisper/ETA 等不构成能力合同 | `5ec8714ce8aeccf5da2df3906e36e4ed255eb295875b0ddd71653c2983aa9724` |

## 使用规则

- 参考图用于布局、层级、状态和视觉语言，不要求复刻其中的电影画面、具体字幕文本、文件名或示例数据。
- 不从图中反推未登记的产品能力；与 Requirements/Architecture 冲突时以前者为准。
- 实现验收必须使用实际应用截图与测试 fixture，不能把这些参考图登记成完成证据。
- 若实现必须偏离结构基线，应在对应 Phase Feature 的 evidence 中记录原因、替代方案和验证，而不是直接改写参考图。
