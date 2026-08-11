# SubLift UI 设计入口

本目录是 **Phase 10 macOS Native Subtitle Workbench** 的视觉与交互真源。它把外部设计稿整理为可实施、可验收的项目规范；参考图用于说明方向，不覆盖真实产品能力、运行时契约或需求边界。

## 文档顺序

1. [主设计方案：macOS Subtitle Workbench vNext](macos-workbench-vnext.md)
2. [交互状态与视觉验收规范](interaction-state-spec.md)
3. [现有实现到目标组件的迁移映射](implementation-map.md)
4. [参考资产索引与适用边界](reference-index.md)
5. [Phase 10 架构实施计划](../plans/architecture/phase10-macos-workbench-ui.md)
6. [Phase 10 Feature 跟踪](../phases/phase10.json)

## 权威边界

发生冲突时按以下顺序处理：

1. `docs/REQUIREMENTS.md`、`docs/ARCHITECTURE.md` 与 `docs/cpp/` 的产品/运行时契约；
2. 本目录的主设计方案与交互状态规范；
3. `assets/` 中的参考图；
4. 外部设计说明中的未来设想。

参考图不是像素级实现合同，也不能把图中未实现的能力变成 Phase 10 范围。尤其是自动 OCR 引擎路由、Task Center/批量队列、Whisper、ASS/VTT、模型安装、会话持久化与分发签名，仍按需求和 Phase 10 非目标处理。

## 当前状态

- 设计基线：已整理并冻结。
- Phase 10：已登记，等待按 Feature 顺序进入实际开发。
- 当前产品：仍是 Phase 2/6 已交付的 SwiftUI 开发者 GUI；本目录不宣称视觉升级已经实现。
