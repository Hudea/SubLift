# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-04
- **当前功能：** feat-004 完成（文档收尾），准备 CLI 接入与 Phase 1 端到端验收
- **分支：** main（feat-004 改动未提交）
- **说明：** Phase 1 全部 10 个 feat 完成。代码层 + 文档层就位，仅剩 CLI 接入贯通与真实视频端到端验收门。

## 进行中

- feat-004 改动未提交（等待用户确认提交）

## 近期完成（最近 5 个）

- [x] feat-004：文档收尾（5 份 design 文档 + README + ARCHITECTURE）；详见 phase1.json
- [x] feat-010：字幕导出（SrtExporter format+export + ASS/VTT 占位）
- [x] feat-009：端到端编排与时间轴 pipeline（打轴+去重+串联）
- [x] feat-008：OCR 引擎模块（Mock + Vision PyObjC，默认 zh-Hans+en-US 双语）
- [x] feat-007：字幕区域检测模块（BottomCrop + FixedRegion）

## 阻塞项 / 风险

- [ ] `README.md` 为占位 —— 归入 `feat-004`（置末）
- [ ] `docs/ARCHITECTURE.md` 是空壳 —— 归入 `feat-004`（置末）

## 近期决策

- ADR-0004：任务粒度调整 22→10 粗任务，subtasks 字段承载细节，feat-004 置末
- ADR-0001：Phase 1 模块布局 = 三能力模块(detector/extractor/ocr) + 串联层(pipeline/export) + 入口层(cli)
- ADR-0002：Apple Vision 经 PyObjC 桥接接入，不引入 Swift helper

> 完整决策记录见 `docs/DECISIONS.md`
