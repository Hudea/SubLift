# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-07
- **当前 Phase：** Phase 3 - 优化与基本可用
- **当前功能：** feat-030 前台进度显示与取消（待启动）
- **分支：** main
- **说明：** Phase 3 已完成 feat-029 增量处理、feat-031 打轴优化、feat-033 OCR 字幕层筛选。剩余 feat-027/028 benchmark（在 benchmark 分支）、feat-030 前台进度、feat-032 文档收尾。

## 进行中

- (none)

## 近期完成（最近 5 个）

- [x] feat-033：OCR 字幕层筛选。OcrLine 模型 + OcrResult.lines 向后兼容；SubtitleProfile 数据契约 + IPC schema；selector 纯函数（y 轨道过滤 → 行高过滤 → max_lines 截断 → y 排序）；Pipeline ocr_segment 接入 selector；Swift SubtitleProfileBuilder + GUI 透传。PIL 合成 fixture 端到端验证 selector 过滤背景英文。271 passed + 2 skipped；Swift 123 passed。详见 phase3.json。
- [x] feat-029 + 审查修复 + 增量字幕实时显示：Pipeline 流式 push 模型，bridge 边收帧边处理；GUI editor 监听 extractor.entries 增量同步。
- [x] feat-031：打轴检测优化（patrol 推荐配置已落定）。F1 65%→80.6%（+15.6pp）。详见 phase3.json。
- [x] Phase 3 规划文档建立：`docs/plans/phase3.md`、`docs/phases/phase3.json`。
- [x] feat-026：Phase 2 文档收尾全部完成。

## 阻塞项 / 风险

- [ ] **patrol 过切分**：7 组 GT 被切成两条检测段（OCR 噪声差异导致 dedupe 无法合并）。详见 `docs/HURDLES.md`。
- [ ] **短字幕漏检**：`<=1200ms` 的 GT 只命中 7/16，主因 hysteresis_frames=2 吃掉 40%+ 时长。详见 `docs/HURDLES.md`。
- [ ] **persistent_text_policy 跳过**：feat-033 跨段时序过滤水印/持久背景文字未实现，留作后续迭代。
- [ ] **feat-027/028 benchmark 在 benchmark 分支**：源码未合并到 main，依赖 benchmark 的测试用 importorskip 跳过。
- [ ] **ground truth 素材有限**：目前主要依赖 Zootopia clip。

## 近期决策

- feat-033 selector 用 crop-relative 坐标系：OCR 引擎收裁剪图，bbox 自然在 crop 空间，GUI 生成 profile 时减去 region_box 原点 y。
- feat-033 profile=None 时走旧路径（OcrResult.text/confidence），MockOcrEngine 和现有测试零改动。
- feat-029 Pipeline 用 push 模型而非 pull：feed(frame) -> SegmentEvent | None，bridge 主动推进。
- feat-031 SSIM patrol 记为推荐配置（`enable_ssim_patrol=True, interval=3, threshold=0.92`）。

> 完整决策记录见 `docs/DECISIONS.md`
