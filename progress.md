# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-04
- **当前功能：** feat-010 完成（字幕导出 SRT + ASS/VTT 占位），准备 feat-004（文档收尾）与 CLI 接入
- **分支：** main（feat-010 改动未提交）
- **说明：** Phase 1 代码层全部就位（三能力模块 + pipeline + export），仅剩 feat-004 文档与 CLI 接入/端到端验收门。

## 进行中

- feat-010 改动未提交（等待用户确认提交）

## 近期完成（最近 5 个）

- [x] feat-010：字幕导出（SrtExporter format+export 双方法 + ASS/VTT 占位）；26 新单测全绿，详见 phase1.json
- [x] feat-009：端到端编排与时间轴 pipeline（打轴+去重+串联三子任务）；82 单测全绿，端到端实测 91.3% 召回/0 误检
- [x] feat-008：OCR 引擎模块（Mock + Vision PyObjC，默认 zh-Hans+en-US 双语）
- [x] feat-007：字幕区域检测模块（BottomCrop + FixedRegion）
- [x] feat-006：帧采样模块（FfmpegExtractor + Protocol @runtime_checkable）

## 阻塞项 / 风险

- [ ] `README.md` 为占位 —— 归入 `feat-004`（置末）
- [ ] `docs/ARCHITECTURE.md` 是空壳 —— 归入 `feat-004`（置末）

## 近期决策

- ADR-0004：任务粒度调整 22→10 粗任务，subtasks 字段承载细节，feat-004 置末
- ADR-0001：Phase 1 模块布局 = 三能力模块(detector/extractor/ocr) + 串联层(pipeline/export) + 入口层(cli)
- ADR-0002：Apple Vision 经 PyObjC 桥接接入，不引入 Swift helper

> 完整决策记录见 `docs/DECISIONS.md`
