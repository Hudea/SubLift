# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-04
- **当前功能：** feat-011 完成（CLI 接入 + 端到端验收），Phase 1 全部收官
- **分支：** main（feat-011 改动未提交）
- **说明：** Phase 1 全部 11 个 feat 完成。CLI 可用，端到端跑通，性能 16x 实时。打轴召回率 72.4%/精确率 100%，OCR 字符准确率 22.5%（主因：字幕区域裁剪过宽，英文新闻标题干扰）。

## 进行中

- feat-011 改动未提交（等待用户确认提交）

## 近期完成（最近 5 个）

- [x] feat-011：CLI 接入 + 端到端验收（--engine 参数，8 CLI 单测，1080p 视频跑通，性能 16x 实时，benchmark 两指标 + 逐条对比）
- [x] feat-004：文档收尾（3 份 design 文档 + README + ARCHITECTURE）
- [x] feat-010：字幕导出（SrtExporter format+export + ASS/VTT 占位）
- [x] feat-009：端到端编排与时间轴 pipeline（打轴+去重+串联）
- [x] feat-008：OCR 引擎模块（Mock + Vision PyObjC，默认 zh-Hans+en-US 双语）

## 阻塞项 / 风险

- [ ] 字幕区域裁剪过宽（bottom_ratio=0.3，实际字幕在 80~87% 区域），英文新闻标题干扰 OCR → 准确率低，待调优
- [ ] dHash 对中文判别力不足，漏分段 → 详见 HURDLES
- [ ] OCR 锚帧落过渡画面，空文本 → 详见 HURDLES

## 近期决策

- ADR-0004：任务粒度调整 22→10 粗任务，subtasks 字段承载细节，feat-004 置末
- ADR-0001：Phase 1 模块布局 = 三能力模块(detector/extractor/ocr) + 串联层(pipeline/export) + 入口层(cli)
- ADR-0002：Apple Vision 经 PyObjC 桥接接入，不引入 Swift helper

> 完整决策记录见 `docs/DECISIONS.md`
