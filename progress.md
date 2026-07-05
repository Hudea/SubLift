# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-05
- **当前功能：** Phase 2 macOS GUI;feat-020(拖拽导入+元数据解析)已完成
- **分支：** apps/macos-gui
- **说明：** feat-020 落地：DropZone(.dropDestination) + VideoMetadata(文件名/分辨率/时长/编码/大小) + 元数据栏。整个窗口支持拖拽。下一任务 feat-021（字幕时间轴+列表+编辑）。

## 进行中

- (无,等待开始 feat-021)

## 近期完成（最近 5 个）

- [x] feat-020：拖拽导入 + 视频元数据解析。DropZone(.dropDestination) + VideoMetadataLoader(AVURLAsset/ffprobe 异步解析) + 元数据栏(文件名/分辨率/时长/编码/大小)。73 Swift 测试全绿。
- [x] feat-019：mkv 兜底 + 系统 ffmpeg 检测。FfmpegDetector+MjpegParser(纯函数切帧)+FfmpegFrameSampler。FrameSampler 按扩展名路由。mkv 预览 ffmpeg seek 逐帧。59 Swift 测试全绿。mkv 端到端 47 条字幕 7.2s，与 mp4 结果一致。
- [x] feat-018：AVFoundation 抽帧 + IPC 帧流端到端。FrameSampler（AVAssetReader + JPEG q=85）+ SubtitleExtractor（@MainActor 协调器，IPC 调用 Task.detached 不阻塞 UI）+ entries 只读列表。真实 Vision OCR 120s 视频跑出 50 条字幕。49 Swift 测试全绿。
- [x] feat-017：AVPlayer 视频预览 + 当前帧时间显示。VideoPreview.swift（NSViewRepresentable+AVPlayerLayer，为 feat-022 叠加层留路）+ PlayerModel + VideoControlsView + TimeFormatter。43 Swift 测试全绿。
- [x] feat-016：Python bridge 接入 Pipeline。run_frames() + BridgeHandler(269行) + finalize 消息 + 安全限制。198 Python + 33 Swift 测试全绿。Swift→Python 完整 Pipeline 跨进程跑通。

## 阻塞项 / 风险

- [ ] **增量 OCR 改造（技术债）**：feat-018 采用流式抽帧 + 批量 OCR 务实方案。真增量（首条反馈 ≤10s）需重构 Pipeline 为滑动窗口模型：PixelDiffTimeliner 改为维护滚动状态，Pipeline 新增 process_frame() 增量接口，bridge.py 改为 push 模型。后续 Phase 2b 末或 Phase 3 处理。
- [ ] **路由分发策略待研究**：feat-019 按扩展名分发（.mkv → ffmpeg，其他 → AVFoundation）。avi/flv/wmv 等冷门格式未覆盖。后续可改为「试 AVFoundation 失败再回退 ffmpeg」覆盖全格式，但需权衡 ~1s 失败延迟。
- [ ] 字幕区域裁剪过宽（bottom_ratio=0.3，实际字幕在 80~87% 区域），英文新闻标题干扰 OCR → 准确率低，待调优
- [ ] dHash 对中文判别力不足，漏分段 → 详见 HURDLES
- [ ] OCR 锚帧落过渡画面，空文本 → 详见 HURDLES
- [ ] feat-025 公证（embedded Python + hardened runtime）风险延后处理，先做 UI

## 近期决策

- ADR-0007c/d(独立决策):MsgPack 评估后撤销,继续用 JSON;不影响 feat-015 任务范围(7 类消息 schema 仍需定义)
- ADR-0007:Phase 2 GUI 三项设计决策(run_frames 帧流接入 / Swift 端 SRT)
- ADR-0004:任务粒度调整 22→10 粗任务,subtasks 字段承载细节,feat-004 置末

> 完整决策记录见 `docs/DECISIONS.md`
