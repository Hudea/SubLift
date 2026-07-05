# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-05
- **当前功能：** Phase 2 macOS GUI
- **分支：** apps/macos-gui
- **说明：** feat-024（SRT 导出）已完成。等待决定下一个任务（建议 feat-025 .app 打包 + 公证 spike，或先推进 feat-026 文档收尾）。

## 进行中

- 无

## 近期完成（最近 5 个）

- [x] feat-024：SRT 导出（Swift 端直接格式化 + NSSavePanel）。117 Swift 测试全绿（含真实文件 I/O 测试），`./init.sh` 8/8 通过。

- [x] feat-023：引擎选择。增加 SettingsView 并使用 `@AppStorage`，同时在 ContentView 的提取按钮旁放置 Picker 用于快速切换。

- [x] feat-022：Vision 字幕区域检测 + 预览多选 + IPC 接线。111 Swift 测试全绿;bridge region_box 单测 3 条。
- [x] feat-021：字幕时间轴 + 列表 + 编辑。SubtitleEntry(UUID+var) + SubtitleEditor(load/updateText/merge/split/currentId) + SubtitleList(List+双击编辑文本+合并/拆分)。HSplitView 布局。88 Swift 测试全绿。
- [x] feat-020：拖拽导入 + 视频元数据解析。DropZone(.dropDestination) + VideoMetadataLoader(AVURLAsset/ffprobe 异步解析) + 元数据栏。73 Swift 测试全绿。
- [x] feat-019：mkv 兜底 + 系统 ffmpeg 检测。FfmpegDetector+MjpegParser+FfmpegFrameSampler。mkv 端到端 47 条 7.2s，与 mp4 一致。59 Swift 测试全绿。
- [x] feat-018：AVFoundation 抽帧 + IPC 帧流端到端。FrameSampler + SubtitleExtractor + entries 列表。120s 视频跑出 50 条字幕。49 Swift 测试全绿。

## 阻塞项 / 风险

- [ ] **增量 OCR 改造（技术债）**：feat-018 采用流式抽帧 + 批量 OCR 务实方案。真增量（首条反馈 ≤10s）需重构 Pipeline 为滑动窗口模型：PixelDiffTimeliner 改为维护滚动状态，Pipeline 新增 process_frame() 增量接口，bridge.py 改为 push 模型。后续 Phase 2b 末或 Phase 3 处理。
- [ ] **路由分发策略待研究**：feat-019 按扩展名分发（.mkv → ffmpeg，其他 → AVFoundation）。avi/flv/wmv 等冷门格式未覆盖。后续可改为「试 AVFoundation 失败再回退 ffmpeg」覆盖全格式，但需权衡 ~1s 失败延迟。
- [ ] 字幕区域裁剪过宽 → feat-022 已接入提取；Zootopia 类样本 E2E OCR 改善待手动 benchmark
- [ ] dHash 对中文判别力不足，漏分段 → 详见 HURDLES
- [ ] OCR 锚帧落过渡画面，空文本 → 详见 HURDLES
- [ ] feat-025 公证（embedded Python + hardened runtime）风险延后处理，先做 UI

## 近期决策

- ADR-0008:feat-022 改为 Vision 候选框+用户多选;Y 由选中框推算,X 全宽;取消手动画框
- ADR-0007c/d(独立决策):MsgPack 评估后撤销,继续用 JSON;不影响 feat-015 任务范围(7 类消息 schema 仍需定义)
- ADR-0007:Phase 2 GUI 三项设计决策(run_frames 帧流接入 / Swift 端 SRT)
- ADR-0004:任务粒度调整 22→10 粗任务,subtasks 字段承载细节,feat-004 置末

> 完整决策记录见 `docs/DECISIONS.md`
