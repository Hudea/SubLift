# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-07
- **当前 Phase：** Phase 3 - 优化与基本可用
- **当前功能：** feat-030 前台进度显示与取消（待启动）
- **分支：** main
- **说明：** benchmark 分支已合入 main。Phase 3 已完成 feat-027 benchmark 框架、feat-031 打轴优化、feat-033 OCR 字幕层筛选、feat-034 持久背景文字过滤；feat-028 基线入库已按用户决定标记 blocked；剩余 feat-030 前台进度与取消、feat-032 文档收尾。

## 进行中

- (none)

## 近期完成（最近 5 个）

- [x] feat-027：Benchmark 框架。新增诊断指标、一对一时间匹配、agent JSON、GT/detection CSV 与 summary Markdown；manifest 一键入口可用。详见 phase3.json。
- [x] benchmark 产物建档：基于最新 git commit 信息自动生成 label，并在 output_dir 下自动累加序号子目录。
- [x] feat-034：持久背景文字过滤。PersistentTextPolicy dataclass + SubtitleProfile 扩展 + IPC schema；Pipeline 缓存 per-segment OcrLine；persistent.py 纯函数双条件算法（A 同文本连续重复 ≥K1，B 同 y_bin 不同文本 ≥M，排除目标轨道）；finalize 接入；Swift GUI 默认生成 policy。303 passed + 2 skipped；Swift 127 passed。详见 phase3.json。
- [x] feat-033：OCR 字幕层筛选。OcrLine 模型 + selector 纯函数 + SubtitleProfile + Swift 接入。
- [x] feat-029 + 审查修复 + 增量字幕实时显示：Pipeline 流式 push 模型。

## 阻塞项 / 风险

- [ ] **feat-028 跳过 / blocked**：基线不另做入库产物；已有会话跑测记录见 `docs/benchmarks/baseline.md`，后续对比可临时跑 manifest。
- [ ] **patrol 过切分**：7 组 GT 被切成两条检测段（OCR 噪声差异导致 dedupe 无法合并）。详见 `docs/HURDLES.md`。
- [ ] **短字幕漏检**：`<=1200ms` 的 GT 只命中 7/16，主因 hysteresis_frames=2 吃掉 40%+ 时长。详见 `docs/HURDLES.md`。
- [ ] **ground truth 素材有限**：目前主要依赖 Zootopia clip。
- [ ] **persistent filter 已知限制**：ticker 与目标字幕 y 中心完全重合且在同 y_bin 时无法区分；y_bin_ratio 阈值需在真实素材上调参。

## 近期决策

- feat-028 跳过：基线不入库，需对比时临时跑 manifest；历史跑测摘要保留在 `docs/benchmarks/baseline.md`。
- benchmark 诊断报告作为后续优化口径：打轴看 timing，识别看 recognition，产品可用性看 e2e。
- feat-034 persistent filter 在 Pipeline.finalize 后处理：时序统计完整、不增加 push_entry 延迟、is_final 全量替换机制契合。

> 完整决策记录见 `docs/DECISIONS.md`
