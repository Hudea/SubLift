# macOS Subtitle Workbench

## 1. 产品定位

Workbench 面向单个本地视频的“导入 → 选区 → 提取 → 校对 → 导出”闭环。界面遵循原生 macOS
窗口、Toolbar、菜单、Inspector、文件面板和键盘行为，不把产品组织成 Dashboard。

每个窗口由一个 `WorkspaceModel` 协调 video、metadata、player、region、extraction、
transcript、selection 与 command availability；OCR、IPC、坐标和 SRT 逻辑继续由 focused Core models 负责。

## 2. 信息架构

```text
Window
├── Toolbar：打开、区域、提取/停止、导出、Inspector
├── Video Workspace
│   ├── Video Canvas / Region Overlay
│   ├── Transport / Timeline
│   └── Quick Extraction Settings
├── Transcript Panel
└── Context Inspector：Video / Region / Extraction / Subtitle
```

- 默认内容比例以视频为主，Transcript 保持可读宽度。
- Inspector 是可关闭的上下文面板；窄窗口优先回收 Inspector，再压缩 Transcript。
- Settings 使用独立窗口，不占用 Inspector。
- Task Center 使用独立窗口，不把批量队列塞入 Workspace。

## 3. Surface

| 状态 | 主要表现 | 主动作 |
|---|---|---|
| Empty | Welcome、格式说明与本机处理说明 | Open |
| Loading | 保留窗口骨架并阻止重复导入 | 等待或安全取消 |
| Ready | 视频、区域摘要与空/既有 Transcript | Extract |
| Region Editing | 视频 Overlay、候选区与 Region Inspector | Done |
| Starting / Processing / Finalizing | 真实进度、Stop 与只读 Live Transcript | Stop |
| Review | 最终字幕、Timeline、编辑与 Subtitle Inspector | Export |
| Failed / Cancelled | 保留可恢复上下文和可操作错误 | Retry / Extract Again |

拖入高亮是 View 的瞬时反馈，不是持久 Workspace 状态。

## 4. Transcript 与播放

- Processing/Finalizing 期间可选择、搜索和 seek，但不得编辑、拆分、合并或导出。
- 最终 `entries` 到达后一次性进入 Review；它替换增量预览并成为唯一可编辑数据源。
- 当前播放高亮、用户选择和搜索命中是独立状态。
- 选择字幕可切换 Inspector mode，但不得强制打开用户已关闭的 Inspector。
- Timeline、Transcript 与播放器共享同一时间语义；编辑后条数、选择和时码保持一致。

## 5. Region 与 Inspector

- Region Editing 只修改字幕区域；OCR 和提取配置不在 Overlay 内实现。
- Video Inspector 展示文件与媒体 metadata。
- Region Inspector 展示候选、选中区域和必要几何信息。
- Extraction Inspector 展示当前/最近一次配置、runtime 与进度，不复制设置控件。
- Subtitle Inspector 只在最终结果允许编辑时提供文本和时码操作。

## 6. 配置与提取

- Settings 与 Quick Extraction Settings 共享引擎、采样等级等偏好。
- 每次提取创建 active 配置快照；运行中修改偏好只标记下次生效。
- Review 保留 final 配置身份；重新提取必须显式触发。
- UI 只展示 Worker 真实 capability；缺失引擎时给出错误，不静默换引擎或回退 Python。

## 7. 命令与视觉

- Toolbar、菜单、上下文菜单和快捷键使用同一个 command availability。
- 主动作保持单一；禁用状态同时具有视觉反馈和 action guard。
- 使用系统语义色、字体、材质与焦点行为；视频区域保持稳定黑底。
- Light/Dark、960×600 紧凑窗口、键盘操作和辅助功能标签均属于设计合同。

详细状态和命令矩阵见 [交互状态规范](interaction-state-spec.md)；实际截图见
[`evidence/`](evidence/)。

## 8. 非目标

- 不在 Workbench 中实现批量队列、并行 OCR、Whisper、自动引擎切换或模型下载器。
- 不展示无法由运行时提供的 ETA、平均置信度或虚假 capability。
- 当前导出只提供 SRT；ASS/VTT 占位不构成 UI 能力。
- 不从参考图复制示例媒体、字幕文本或未登记功能。
