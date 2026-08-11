# 现有实现到目标组件的迁移映射

Phase 10 是 UI/状态编排升级，不重写提取算法。迁移应先建立 Session 状态与 Native Shell，再逐步搬移现有 View；每个 Feature 结束时应用仍可构建和运行。

## 1. 现有能力保留

| 现有对象 | 保留职责 | Phase 10 调整 |
|---|---|---|
| `PlayerModel` / `VideoPreview` | AVPlayer 与 MKV fallback、play/seek | 由 WorkspaceModel 协调；View 重组为 VideoCanvas + TransportBar |
| `SubtitleExtractor` | 单 job 生命周期、progress、push_entry、final entries、取消 | 状态投影到 Workspace；补充处理期只读合同，不改 IPC |
| `SubtitleEditor` | 最终字幕编辑、selection/current、split/merge | 仅 Review 开放 mutation；为 commands/Inspector 提供统一 action |
| `VideoMetadataLoader` | 文件 metadata | 从 metadataBar 移到 Video Inspector |
| `RegionSelectionModel` | 候选、选择、merged region、重检 | 进入显式 Region Editing 状态；几何信息渐进披露 |
| `RuntimePolicy` / `PipelineClient` | C++ 默认、Python 显式回滚、fail-closed | UI 只显示真实选择，不实现自动 fallback |
| `SrtFormatter` / Save Panel | SRT 导出 | 移到 Toolbar/Menu command，保持单步导出 |
| `SamplingQuality` | 快速/平衡/精细 → fps | Settings/Extraction Inspector 使用，普通 UI 不显示 raw fps |

## 2. 目标模块

```text
apps/macos/Sources/SubLiftMac/
  App/
    SubLiftMacApp.swift
    WorkspaceCommands.swift
  Core/
    WorkspaceModel.swift
    WorkspaceState.swift
    (existing focused models remain)
  UI/
    Workspace/
      WorkspaceRootView.swift
      WorkspaceToolbar.swift
      WorkspaceInspector.swift
    Welcome/
      WelcomeView.swift
      DropTargetView.swift
    Media/
      VideoCanvas.swift
      TransportBar.swift
      SubtitleTimeline.swift
    Region/
      RegionOverlay.swift
      RegionInspector.swift
    Transcript/
      TranscriptPanel.swift
      TranscriptRow.swift
      SubtitleInspector.swift
    Extraction/
      ExtractionProgressView.swift
      ExtractionInspector.swift
    Settings/
      SettingsRootView.swift
      GeneralSettingsView.swift
      RecognitionSettingsView.swift
      AdvancedSettingsView.swift
```

实际实施可根据 Swift target 规模合并小文件，但依赖方向和职责不可退回到单个超大 `ContentView`。

## 3. 状态所有权

`WorkspaceModel` 负责一个 Window 的 Session coordination：

- video URL 与导入生命周期；
- metadata、player、region、extractor、editor 的组合；
- workspace surface、Inspector mode、Transcript read-only/editable；
- Open/Extract/Stop/Region/Export/Subtitle commands 的 availability；
- 新视频、取消、失败、重试和最终结果切换。

Focused Core model 仍保留自身业务逻辑。`WorkspaceModel` 不复制 OCR、IPC、坐标、编辑或格式化实现。

## 4. 迁移策略

1. 先增加 `WorkspaceState/WorkspaceModel` 和状态测试，旧 UI 仍可运行。
2. 接入 Welcome、拖拽和 Native Workspace shell，复用旧 View。
3. 建立 Inspector 容器并搬入 metadata，再迁 Region controls。
4. 重构 Transcript 为 read-only/editable 双阶段和 contextual commands。
5. 加入 Timeline 与 Settings 分层。
6. 完成响应式、Dark/Contrast/VoiceOver/Keyboard 证据，再删除已经没有引用的旧 UI 组合代码。

任何一步都不得把 Worker 或 Pipeline 业务移入 SwiftUI View。

## 5. 参考图与真实实现差异

| 参考表现 | 项目事实 | 实施规则 |
|---|---|---|
| “自动（推荐）”引擎 | F10 尚未实现多引擎自动路由 | Phase 10 不提供虚假 Automatic；保留未来位置 |
| Whisper 引擎 | 当前只有 Vision/Paddle/Mock | 不展示，不加入数据模型 |
| Task Center | F24 未实现 | 第 8 图仅未来参考，不建入口 |
| 剩余时间/平均置信度 | 当前提取状态不能可靠提供全部值 | 只显示 progress、frame count、processingRate、runtimeIdentity 等真实数据 |
| Settings 中安装模型 | 当前没有产品化下载器 | 只显示 capability/缺失错误，不做假按钮 |
| ASS/VTT | 仅接口占位 | Export 只提供 SRT |
| 时间精调控件 | `SubtitleEditor` 已有底层步进逻辑，但目标交互需单独验证 | 只在对应 Feature 明确接线并测试后展示 |
| 多窗口拖入新工作区 | `WindowGroup` 存在，但当前导入只替换当前 URL | Phase 10 不承诺自动新建窗口，文案按真实行为 |

## 6. 删除与兼容边界

- 旧 `ContentView`、`DropZone`、`SubtitleList` 等只在替代组件接线并通过测试后移除或改为薄包装。
- 保留 legacy frame mode、UDS message、C++/Python runtime policy 与现有 engine raw values。
- UserDefaults key 如需迁移必须提供兼容读取测试；不能静默把 `.mock` 改写成正式引擎。
- 不修改参考资产来“匹配实现”；实现差异在文档和验收 evidence 中明确记录。
