# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-03
- **当前功能：** feat-003 完成，准备 feat-004（项目文档与架构填充）
- **分支：** main（commit 9f7e1fb，feat-001/002/003 改动未提交）
- **说明：** Phase 1 实现进行中。任务粒度已从 22 细任务合并为 10 粗任务（feat-001~010），细节移入 subtasks 字段。feat-001/002/003 完成，下一步 feat-004。

## 进行中

- feat-004：项目文档与架构填充（待开始）

## 近期完成（最近 5 个）

- [x] 任务粒度调整：22 细任务 → 10 粗任务（feat-001~010），subtasks 字段承载细节
- [x] feat-003：init.sh 环境检查；./init.sh 退出 0
- [x] feat-002：包结构骨架；python -m sublift 退出 0；ruff/mypy/pytest 全绿
- [x] feat-001：pyproject.toml + uv 初始化；uv sync + import sublift 全绿
- [x] 规划落档：docs/plans/phase1.md + feature-list.json + docs/phases/phase1.json

## 阻塞项 / 风险

- [ ] `README.md` 不存在 —— 归入 `foundation.readme` 任务
- [ ] `init.sh` 是空壳 —— 归入 `foundation.init_sh` 任务
- [ ] `docs/ARCHITECTURE.md` 是空壳 —— 归入 `foundation.architecture` 任务
- [ ] `pyproject.toml` 不存在 —— 归入 `foundation.pyproject` 任务

## 近期决策

- ADR-0001：Phase 1 模块布局 = 三能力模块(detector/extractor/ocr) + 串联层(pipeline/export) + 入口层(cli)
- ADR-0002：Apple Vision 经 PyObjC 桥接接入，不引入 Swift helper
- ADR-0003：Phase 1 范围 = 可运行 MVP（真实视频→SRT），非仅抽象底座

> 完整决策记录见 `docs/DECISIONS.md`
