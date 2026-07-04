# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-05
- **当前功能：** Phase 2 macOS GUI;feat-015(IPC 协议 7 类消息 schema)已完成
- **分支：** apps/macos-gui
- **说明：** feat-015 落地：7 类 IPC 消息 JSON schema（Python protocol.py + Swift Messages.swift Codable struct）+ server.py handler stub 响应分发 + 跨语言 fixture 单测。顺带改 Phase 1 SubtitleEntry 加 confidence。下一任务 feat-016（Python bridge 包装 Pipeline，接入 run_frames）。

## 进行中

- (无,等待开始 feat-016)

## 近期完成（最近 5 个）

- [x] feat-015：IPC 协议 7 类消息 schema(JSON 序列化)。Python protocol.py(215 行) + Swift Messages.swift(245 行) + server.py handler stub 分发 + 48 Python + 18 Swift 单测。顺带改 Phase 1 SubtitleEntry 加 confidence(默认 1.0,向后兼容)。全量验证 177 passed + 29 passed。
- [x] feat-015(MsgPack 评估):两个 Swift MsgPack 库都有嵌套解码 bug,跨语言测试暴露;决策继续用 JSON(ADR-0007c/d)。注:此为决策性产物,7 类消息 schema 定义重开为 feat-015 实质任务
- [x] feat-014：Python UDS service 启动骨架(server.py 184 行 + PipelineClient.swift 221 行,10 Python + 9 Swift 测试,跨进程握手验证通过)
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
