# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-07
- **当前 Phase：** Phase 3 - 优化与基本可用
- **当前功能：** 无（已完成 commit-based 自动 Label 与序号累加子文件夹功能）
- **分支：** benchmark
- **说明：** Phase 3 规划已建立，目标限定为三项：benchmark 优化、增量处理与前台进度优化、打轴检测优化。OCR 区域裁剪、ASS/VTT、PaddleOCR、.app 打包等明确后置。feat-028 基线入库已跳过，基线以会话跑测形式记于 docs/benchmarks/baseline.md。

## 进行中

- 无

## 近期完成（最近 5 个）

- [x] 实现 commit-based 自动 Label 与测试序号自动递增建档：基于最新 git commit 信息自动过滤提取 6 字 label，并扫描 output_dir 下已有子文件夹自动累加序号（如 `字幕层筛选_1`），将四份报告产物收纳于专属子文件夹下，测试全绿。
- [x] benchmark 产物目录调整：默认输出从 `benchmark/reports` 改为 `debug/benchmark-reports`，`debug/` 已在 `.gitignore` 中忽略；manifest、示例、文档与测试同步更新；`./init.sh` 8/8 通过。
- [x] feat-027：Benchmark 框架完成。新增一对一打轴匹配、timing/recognition/e2e 指标、agent JSON、GT/detection cases CSV 与 summary Markdown；旧宽松对齐和 WER 主口径已移除。
- [x] feat-031：打轴检测优化（独立任务，patrol 推荐配置已落定）。F1 65%→80.6%（+15.6pp），recall 60.9%→90.8%，precision 不下降，merged_into_neighbor FN 减少约 70%。详见 phase3.json。
- [x] Phase 3 规划文档建立：`docs/plans/phase3.md`、`docs/phases/phase3.json`，更新 `feature-list.json`。

## 阻塞项 / 风险

- [ ] **feat-028 跳过**：基线入库不做，下游 feat-029/031 若需对比可临时跑 manifest。
- [ ] **patrol 过切分**：7 组 GT 被切成两条检测段（OCR 噪声差异导致 dedupe 无法合并）。patrol 阈值 0.92 过敏感，后续可调到 0.88~0.90 或加 dedupe 模糊合并。详见 `docs/HURDLES.md`。
- [ ] **短字幕漏检**：`<=1200ms` 的 GT 只命中 7/16，主因是 hysteresis_frames=2 吃掉 40%+ 时长 + 单字字幕前景占比不足。后续可 hysteresis=1 + 降 presence_threshold，但需配合 patrol 阈值调优避免 FP。详见 `docs/HURDLES.md`。
- [ ] **Phase 3 范围控制**：用户明确先不规划具体实现方法，需在每个 feat 启动时重新评估技术方案，避免范围发散。
- [ ] **增量处理重构风险**：允许修改 pipeline/，可能引入回归；必须先完成 benchmark 基线再动手重构。
- [ ] **ground truth 素材有限**：目前主要依赖 Zootopia clip，benchmark 说服力可能不足。

## 近期决策

- feat-028 跳过：基线不入库，需对比时临时跑 manifest。
- benchmark 诊断报告后续作为唯一优化口径：打轴看 timing，识别看 recognition，产品可用性看 e2e。
- feat-031 SSIM patrol 记为推荐配置（`enable_ssim_patrol=True, interval=3, threshold=0.92`），当前默认开启。F1 +15.6pp 但未达 95% 目标。
- feat-031 属于项目打轴优化，不归入 benchmark 优化；规划顺序为 trace 归因 → SSIM patrol 补强连续字幕 CHANGE → 短字幕 IN/OUT 专项。

> 完整决策记录见 `docs/DECISIONS.md`
