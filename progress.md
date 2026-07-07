# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-07
- **当前 Phase：** Phase 3 - 优化与基本可用
- **当前功能：** feat-030 前台进度显示与取消（待启动）
- **分支：** main
- **说明：** Phase 3 已完成 feat-029 增量处理、feat-031 打轴优化、feat-033 OCR 字幕层筛选、feat-034 持久背景文字过滤。剩余 feat-027/028 benchmark（在 benchmark 分支）、feat-030 前台进度、feat-032 文档收尾。

## 进行中

- (none)

## 近期完成（最近 5 个）

- [x] feat-034：持久背景文字过滤。PersistentTextPolicy dataclass + SubtitleProfile 扩展 + IPC schema；Pipeline 缓存 per-segment OcrLine；persistent.py 纯函数双条件算法（A 同文本连续重复 ≥K1，B 同 y_bin 不同文本 ≥M，排除目标轨道）；finalize 接入；Swift GUI 默认生成 policy。303 passed + 2 skipped；Swift 127 passed。详见 phase3.json。
- [x] feat-033：OCR 字幕层筛选。OcrLine 模型 + selector 纯函数 + SubtitleProfile + Swift 接入。
- [x] feat-029 + 审查修复 + 增量字幕实时显示：Pipeline 流式 push 模型。
- [x] feat-031：打轴检测优化（patrol 推荐配置已落定）。F1 65%→80.6%（+15.6pp）。
- [x] Phase 3 规划文档建立：`docs/plans/phase3.md`、`docs/phases/phase3.json`。

## 阻塞项 / 风险

- [ ] **patrol 过切分**：7 组 GT 被切成两条检测段（OCR 噪声差异导致 dedupe 无法合并）。详见 `docs/HURDLES.md`。
- [ ] **短字幕漏检**：`<=1200ms` 的 GT 只命中 7/16，主因 hysteresis_frames=2 吃掉 40%+ 时长。详见 `docs/HURDLES.md`。
- [ ] **feat-027/028 benchmark 在 benchmark 分支**：源码未合并到 main，依赖 benchmark 的测试用 importorskip 跳过。
- [ ] **ground truth 素材有限**：目前主要依赖 Zootopia clip。
- [ ] **persistent filter 已知限制**：ticker 与目标字幕 y 中心完全重合且在同 y_bin 时无法区分；y_bin_ratio 阈值需在真实素材上调参。

## 近期决策

- feat-034 persistent filter 在 Pipeline.finalize 后处理：时序统计完整、不增加 push_entry 延迟、is_final 全量替换机制契合。
- feat-034 双条件算法排除目标轨道：target_bins 由 selector 选出目标行的 y_bin 集合，条件 A/B 只在非目标轨道判定，避免误伤多变/重复的目标字幕。
- feat-034 指纹 = (归一化文本, y_bin)，y_bin = center_y / (line_height * y_bin_ratio)，同 bin 视为同 y 轨道。

> 完整决策记录见 `docs/DECISIONS.md`
