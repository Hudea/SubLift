# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-28（6.2 phase-review 加固落地）
- **当前 Phase：** phase6-native-cpp-core
- **子阶段 6.0 / 6.1 / 6.2：** **done**（含 review 加固：call_count / BGR 色带 / OpenCV hard-require 等）
- **下一刀：** **6.3** Extractor 设计与 `feat-063xx` 登记
- **分支：** `refactor/cpp`
- **说明：** 产品默认路径仍为 Python。

## 进行中

- 无（6.2 加固已写入工作区待提交后进入 6.3 设计提交）

## 近期完成（最近 5 个）

- [x] **phase-review 6.2 加固**：Mock call_count；chromatic BGR golden；merge scenario；OpenCV require；API 禁拷贝；ctest 117/117
- [x] **feat-06205** Pipeline 端到端 golden harness（现 8 scenarios）
- [x] **feat-06204** Pipeline::finalize + cancel parity
- [x] **feat-06203** Pipeline::ocr_segment 双路径 parity
- [x] **feat-06202** Pipeline::feed 打轴路径 parity

## 阻塞项 / 风险

- [ ] **merged residual / #15 砰**
- [ ] **ground truth 素材有限**
- [ ] **Paddle cutover**：6.6 后 paddle 仍 Python worker
- [ ] **6.3 ffmpeg subprocess**：需复刻 Python extractor 的 ROI crop-before-Python 路径与 path/frame mode

## 近期决策

- **6.2 review**：fixed-mode call_count 必须计数；色带 fixture 才能锁 RGB→BGR
- **6.2 完成**：feed/ocr_segment/finalize/cancel + e2e golden 全 parity，产品路径不切换
- **6.2 OCR**：仅 Mock；Vision 归 6.4；产品路径不切换

> 完整决策记录见 `docs/DECISIONS.md`
