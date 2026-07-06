# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-06
- **当前功能：** Phase 2 macOS GUI 文档收尾已完成
- **分支：** apps/macos-gui
- **说明：** feat-026 文档收尾全部完成。Phase 2 所有计划内功能（feat-012~024）及文档（feat-026）均已完成；feat-025（.app 打包 + 公证）已按用户决策跳过。仓库处于可交付状态。

## 进行中

- 无

## 近期完成（最近 5 个）

- [x] feat-026：Phase 2 文档收尾。更新 plans/phase2.md、ARCHITECTURE.md、REQUIREMENTS.md、DECISIONS.md（ADR-0005/0006）、README.md、新建 design/macos-gui.md。`./init.sh` 8/8 通过。
- [x] feat-024：SRT 导出（Swift 端直接格式化 + NSSavePanel）。117 Swift 测试全绿（含真实文件 I/O 测试）。
- [x] feat-023：引擎选择。增加 SettingsView 并使用 `@AppStorage`，同时在 ContentView 的提取按钮旁放置 Picker 用于快速切换。
- [x] feat-022：Vision 字幕区域检测 + 预览多选 + IPC 接线。111 Swift 测试全绿;bridge region_box 单测 3 条。
- [x] feat-021：字幕时间轴 + 列表 + 编辑。SubtitleEntry(UUID+var) + SubtitleEditor + SubtitleList。HSplitView 布局。88 Swift 测试全绿。
- [x] feat-020 及更早：见 `docs/phases/phase2.json`。

## 阻塞项 / 风险

- [ ] **增量 OCR 改造（技术债）**：feat-018 采用流式抽帧 + 批量 OCR 务实方案。真增量（首条反馈 ≤10s）需重构 Pipeline 为滑动窗口模型：PixelDiffTimeliner 改为维护滚动状态，Pipeline 新增 process_frame() 增量接口，bridge.py 改为 push 模型。后续 Phase 2b 末或 Phase 3 处理。
- [ ] **路由分发策略待研究**：feat-019 按扩展名分发（.mkv → ffmpeg，其他 → AVFoundation）。avi/flv/wmv 等冷门格式未覆盖。后续可改为「试 AVFoundation 失败再回退 ffmpeg」覆盖全格式，但需权衡 ~1s 失败延迟。
- [ ] 字幕区域裁剪过宽 → feat-022 已接入提取；Zootopia 类样本 E2E OCR 改善待手动 benchmark
- [ ] dHash 对中文判别力不足，漏分段 → 详见 HURDLES
- [ ] OCR 锚帧落过渡画面，空文本 → 详见 HURDLES

## 近期决策

- ADR-0009:移除 Phase 2 .app 打包与 notarization 流程（feat-025 跳过）
- ADR-0008:feat-022 改为 Vision 候选框+用户多选;Y 由选中框推算,X 全宽;取消手动画框
- ADR-0007c/d(独立决策):MsgPack 评估后撤销,继续用 JSON;不影响 feat-015 任务范围(7 类消息 schema 仍需定义)

> 完整决策记录见 `docs/DECISIONS.md`
