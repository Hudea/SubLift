# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-28（Phase 6.4 Vision ObjC++ adapter 全部完成）
- **当前 Phase：** phase6-native-cpp-core
- **子阶段 6.0 / 6.1 / 6.2 / 6.3 / 6.4：** **done**（6.4 含 Vision parity harness，ctest 139）
- **子阶段 6.5：** **not-started**（下一阶段 6.5 Worker / Cutover）
- **下一刀：** **Phase 6.5 规划与拆分**
- **分支：** `refactor/cpp`
- **说明：** 产品默认路径仍为 Python。Vision 文本 **L4**；cutover 归 6.6。

## 进行中

- [ ] **Phase 6.5**（下一阶段：Worker / Cutover 规划）

## 近期完成（最近 5 个）

- [x] **feat-06405** Vision parity harness (dump_vision.py + vision.v1.json + Catch2 parity + init.sh)
- [x] **feat-06404** 语言配置 + macOS 集成烟测 (CoreText PingFangSC + [vision][integration] + README)
- [x] **feat-06403** VisionOcrEngine::recognize 主路径 (CGImage RAII + @autoreleasepool + line sorting)
- [x] **feat-06402** CMake / availability 骨架 (is_vision_available + VisionOcrEngine Pimpl)
- [x] **feat-06401** 纯契约（Vision 归一化 box → 像素 + clamp + 排序，bankers_round）
- [x] **feat-06303** FfmpegExtractor 全帧 extract + cancel

## 阻塞项 / 风险

- [ ] **merged residual / #15 砰**
- [ ] **ground truth 素材有限**
- [ ] **Paddle cutover**：6.6 后 paddle 仍 Python worker
- [ ] **6.4 Vision**：live 文本非确定性 → 不把 L0 文本当 init 硬门；坐标变换必须 L0

## 近期决策

- **6.4 五刀：** box 纯契约 → CMake/availability → recognize → 语言+集成 → parity harness
- **6.4 比较层：** box/排序 L0；Vision 文本 L4；GT L3 留给 6.6
- **6.4 仍不 cutover**；实现仅 `sublift_vision_macos`（.mm）

> 完整决策记录见 `docs/DECISIONS.md`
