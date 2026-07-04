# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-04
- **当前功能：** Phase 2 macOS GUI 进行中;feat-013(双工程 + Package.swift)已完成
- **分支：** apps/macos-gui
- **说明：** apps/macos/ SwiftUI 工程骨架落地,swift build + swift test 全绿,Python 端无回归。下一任务 feat-014(Python UDS service 启动骨架)。三项设计决策已落档(ADR-0007a/b/c:Pipeline run_frames / Swift 端 SRT / Swift 手写 MsgPack)。

## 进行中

- (无,等待开始 feat-014)

## 近期完成（最近 5 个）

- [x] feat-013：双工程结构 + Package.swift(apps/macos/ SwiftUI 工程,swift build + test 全绿,Python 116 passed 无回归)
- [x] feat-012：AVFoundation 抽帧 spike(apps/macos/spikes/avf-spike/,6 素材实测,evidence 见 phase2.json)
- [x] feat-011：CLI 接入 + 端到端验收(--engine 参数,8 CLI 单测,1080p 视频跑通,性能 16x 实时)
- [x] feat-004：文档收尾(3 份 design 文档 + README + ARCHITECTURE)
- [x] feat-010：字幕导出(SrtExporter format+export + ASS/VTT 占位)

## 阻塞项 / 风险

- [ ] 字幕区域裁剪过宽（bottom_ratio=0.3，实际字幕在 80~87% 区域），英文新闻标题干扰 OCR → 准确率低，待调优
- [ ] dHash 对中文判别力不足，漏分段 → 详见 HURDLES
- [ ] OCR 锚帧落过渡画面，空文本 → 详见 HURDLES
- [ ] feat-025 公证（embedded Python + hardened runtime）风险延后处理，先做 UI

## 近期决策

- ADR-0007：Phase 2 GUI 三项设计决策(run_frames 帧流接入 / Swift 端 SRT 导出 / Swift 手写 MsgPack,2026-07-04)
- ADR-0004：任务粒度调整 22→10 粗任务,subtasks 字段承载细节,feat-004 置末
- ADR-0001：Phase 1 模块布局 = 三能力模块(detector/extractor/ocr) + 串联层(pipeline/export) + 入口层(cli)

> 完整决策记录见 `docs/DECISIONS.md`
