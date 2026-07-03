# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-03
- **当前功能：** feat-006 完成，准备 feat-007（字幕区域检测模块 detector）
- **分支：** main（commit baf688e，feat-006 改动未提交）
- **说明：** Phase 1 实现进行中。feat-001~006 完成，extractor 模块就位（FfmpegExtractor + Protocol）。下一步 detector。

## 进行中

- feat-007：字幕区域检测模块 detector（待开始）

## 近期完成（最近 5 个）

- [x] feat-006：帧采样模块（FfmpegExtractor + Protocol @runtime_checkable）；19 单测 + 3 integration 全绿
- [x] feat-005：工具链配置细化（ruff 扩展 RUF/SIM/ANN、mypy 覆盖 tests、pytest coverage+markers）；12 测试全绿
- [x] feat-003：init.sh 环境检查；./init.sh 退出 0
- [x] feat-002：包结构骨架；python -m sublift 退出 0；ruff/mypy/pytest 全绿
- [x] feat-001：pyproject.toml + uv 初始化；uv sync + import sublift 全绿

## 阻塞项 / 风险

- [ ] `README.md` 为占位 —— 归入 `feat-004`（置末）
- [ ] `docs/ARCHITECTURE.md` 是空壳 —— 归入 `feat-004`（置末）

## 近期决策

- ADR-0004：任务粒度调整 22→10 粗任务，subtasks 字段承载细节，feat-004 置末
- ADR-0001：Phase 1 模块布局 = 三能力模块(detector/extractor/ocr) + 串联层(pipeline/export) + 入口层(cli)
- ADR-0002：Apple Vision 经 PyObjC 桥接接入，不引入 Swift helper

> 完整决策记录见 `docs/DECISIONS.md`
