# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-28（6.3 phase-review 加固落地）
- **当前 Phase：** phase6-native-cpp-core
- **子阶段 6.0 / 6.1 / 6.2 / 6.3：** **done**（含 review：plan fallback / detector 断言 / golden 扩面；ctest 132）
- **下一刀：** **6.4** Vision 设计登记与 `feat-06401`
- **分支：** `refactor/cpp`
- **说明：** 产品默认路径仍为 Python。不上 libav。

## 进行中

- 无（6.3 加固待提交后进入 6.4 文档提交）

## 近期完成（最近 5 个）

- [x] **fix(phase6.3)** phase-review 加固：plan_frame_io_pure、detector_type、golden 场景、skip 策略；ctest 132/132
- [x] **feat-06305** Extractor 端到端 golden harness
- [x] **feat-06304** ROI crop + plan_frame_io + BottomCrop/RoiPassthrough
- [x] **feat-06303** FfmpegExtractor 全帧 extract + cancel
- [x] **feat-06302** ffprobe：resolve bin + probe

## 阻塞项 / 风险

- [ ] **merged residual / #15 砰**
- [ ] **ground truth 素材有限**
- [ ] **Paddle cutover**：6.6 后 paddle 仍 Python worker
- [ ] **6.3 deferred：** public FfmpegExtractor process 状态 / nlohmann 公开展示（6.5 前 pimpl）

## 近期决策

- **6.3 review：** display_transform fallback 与 mode=roi 必须有 pure/golden；detector_type 不可空断言
- **6.3 仍 subprocess、不 libav**；产品路径不切换
- **下一子阶段：** 6.4 Vision（ObjC++ / L4 文本）

> 完整决策记录见 `docs/DECISIONS.md`
