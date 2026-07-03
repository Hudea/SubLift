# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-03
- **当前功能：** feat-009 完成，准备 feat-010（字幕导出 export）
- **分支：** main（commit da22b75，feat-009 改动未提交）
- **说明：** Phase 1 实现进行中。三能力模块 + pipeline 全部就位，端到端闭环可跑。下一步 export。

## 进行中

- feat-010：字幕导出 export（待开始）

## 近期完成（最近 5 个）

- [x] feat-009：端到端编排与时间轴 pipeline（打轴+去重+串联三子任务）；82 单测全绿，StubExtractor+MockOcrEngine 闭环验证
- [x] feat-008：OCR 引擎模块（MockOcrEngine 固定+序列双模式 + VisionOcrEngine PyObjC 桥接，默认 zh-Hans+en-US 双语识别）；39 单测 + 3 Vision integration（含中英文真实识别）全绿
- [x] feat-007：字幕区域检测模块（BottomCropDetector 比例模式 + FixedRegionDetector 手动模式）；9 单测全绿
- [x] feat-006：帧采样模块（FfmpegExtractor + Protocol @runtime_checkable）；19 单测 + 3 integration 全绿
- [x] feat-005：工具链配置细化（ruff 扩展 RUF/SIM/ANN、mypy 覆盖 tests、pytest coverage+markers）；12 测试全绿

## 阻塞项 / 风险

- [ ] `README.md` 为占位 —— 归入 `feat-004`（置末）
- [ ] `docs/ARCHITECTURE.md` 是空壳 —— 归入 `feat-004`（置末）

## 近期决策

- ADR-0004：任务粒度调整 22→10 粗任务，subtasks 字段承载细节，feat-004 置末
- ADR-0001：Phase 1 模块布局 = 三能力模块(detector/extractor/ocr) + 串联层(pipeline/export) + 入口层(cli)
- ADR-0002：Apple Vision 经 PyObjC 桥接接入，不引入 Swift helper

> 完整决策记录见 `docs/DECISIONS.md`
