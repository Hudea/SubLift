# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-05
- **当前功能：** Phase 2 macOS GUI;feat-016(Python bridge 接入 Pipeline)已完成
- **分支：** apps/macos-gui
- **说明：** feat-016 落地：Pipeline.run_frames() + bridge.py(BridgeHandler) + finalize 消息 + 安全限制(MAX_FRAMES/MAX_JPEG_BYTES)。Swift→Python 端到端跨进程跑通字幕提取（start_job→frame×N→finalize→entries）。下一任务 feat-017（视频预览 AVPlayer）。

## 进行中

- (无,等待开始 feat-017)

## 近期完成（最近 5 个）

- [x] feat-016：Python bridge 接入 Pipeline。run_frames() + BridgeHandler(269行) + finalize 消息 + 安全限制。198 Python + 33 Swift 测试全绿。Swift→Python 完整 Pipeline 跨进程跑通。
- [x] feat-015：IPC 协议 7 类消息 schema(JSON 序列化)。Python protocol.py + Swift Messages.swift + server.py handler stub + 48 Python + 18 Swift 单测。顺带改 Phase 1 SubtitleEntry 加 confidence。
- [x] feat-014：Python UDS service 启动骨架(server.py + PipelineClient.swift,跨进程握手验证通过)
- [x] feat-013：双工程结构 + Package.swift(apps/macos/ SwiftUI 工程,swift build + test 全绿,Python 无回归)
- [x] feat-012：AVFoundation 抽帧 spike(apps/macos/spikes/avf-spike/,6 素材实测,evidence 见 phase2.json)
- [x] feat-011：CLI 接入 + 端到端验收(--engine 参数,8 CLI 单测,1080p 视频跑通,性能 16x 实时)

## 阻塞项 / 风险

- [ ] 字幕区域裁剪过宽（bottom_ratio=0.3，实际字幕在 80~87% 区域），英文新闻标题干扰 OCR → 准确率低，待调优
- [ ] dHash 对中文判别力不足，漏分段 → 详见 HURDLES
- [ ] OCR 锚帧落过渡画面，空文本 → 详见 HURDLES
- [ ] feat-025 公证（embedded Python + hardened runtime）风险延后处理，先做 UI

## 近期决策

- ADR-0007c/d(独立决策):MsgPack 评估后撤销,继续用 JSON;不影响 feat-015 任务范围(7 类消息 schema 仍需定义)
- ADR-0007:Phase 2 GUI 三项设计决策(run_frames 帧流接入 / Swift 端 SRT)
- ADR-0004:任务粒度调整 22→10 粗任务,subtasks 字段承载细节,feat-004 置末

> 完整决策记录见 `docs/DECISIONS.md`
