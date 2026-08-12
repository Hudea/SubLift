# SubLift UI 设计入口

本目录是 SubLift macOS 产品的视觉与交互真源：Phase 10 单视频 Native Workbench 已实施，
Phase 8 批量 Task Center 已完成设计/架构规划、尚未实现。参考图用于说明方向，不覆盖真实产品
能力、运行时契约或需求边界。

## 文档顺序

1. [主设计方案：macOS Subtitle Workbench vNext](macos-workbench-vnext.md)
2. [交互状态与视觉验收规范](interaction-state-spec.md)
3. [现有实现到目标组件的迁移映射](implementation-map.md)
4. [参考资产索引与适用边界](reference-index.md)
5. [Phase 10 架构实施计划](../plans/architecture/phase10-macos-workbench-ui.md)
6. [Phase 10 Feature 跟踪](../phases/phase10.json)
7. [Phase 8 批量任务中心设计合同](batch-task-center.md)
8. [Phase 8 架构实施计划](../plans/architecture/phase8-batch-task-center.md)
9. [Phase 8 Feature 跟踪](../phases/phase8.json)

## 权威边界

发生冲突时按以下顺序处理：

1. `docs/REQUIREMENTS.md`、`docs/ARCHITECTURE.md` 与 `docs/cpp/` 的产品/运行时契约；
2. 本目录的主设计方案与交互状态规范；
3. `assets/` 中的参考图；
4. 外部设计说明中的未来设想。

参考图不是像素级实现合同。Task Center/批量队列已在 Phase 8 独立登记，但在 08102–08410
完成前仍是未实现能力；自动 OCR 引擎路由、Whisper、ASS/VTT、模型安装、Workspace Session
持久化与分发签名仍不在当前范围。

## 当前状态

- 设计基线：已整理并冻结。
- Phase 10：Native Workbench 已按 10001–10415 实施并完成代码、截图与项目门验收；10416 已完成最终文档收口。
- Phase 8：08001 已冻结 Task Center、文件夹扫描、串行队列、安全 SRT、JSON 恢复和 B01–B10；实现从 08102 开始。
- 当前产品：单视频 Native Workbench，包含 Welcome/导入、Video/Transcript Split、Context Inspector、Region Editing、只读 Processing、可编辑 Review、Timeline、分层 Settings 与快速提取设置栏。
- 验收边界：V01–V09、960 紧凑和 Light/Dark 已有实际渲染证据；V10 系统辅助功能设置切换、完整 VoiceOver 会话和部分真实点击受权限限制，详见 10412–10415 evidence。
