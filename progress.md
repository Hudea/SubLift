# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-26（Phase 5.0 PaddleOCR 引擎接入完成）
- **当前 Phase：** phase5-paddle-ocr（已完成）
- **当前功能：** 全部 4 个 feat 已完成（feat-05001 ~ feat-05004）
- **分支：** feat/ocr-model-support（从 main=7a56ce8 切出，worktree /Volumes/lab/pp/SubLift-ocr）
- **说明：** PaddleOCR 第二引擎已接入（rapidocr 3.9.2 + onnxruntime，PP-OCRv6 small）。CLI `--engine paddle`、IPC server、GUI Picker 全链路接线完成。REQUIREMENTS F14 ✅。init.sh 8/8 全绿。

## 进行中

- 无（Phase 5.0 已完成，待用户确认后提交）

## 近期完成（最近 5 个）

- [x] **feat-05004 测试+文档收尾**：test_ocr.py 加 6 个 PaddleOcrEngine 测试；design/ocr.md + README + REQUIREMENTS F14 ✅ + DECISIONS ADR-0018；init.sh 更新装 vision+paddle extras。
- [x] **feat-05003 GUI 引擎选择**：OcrEngineName case paddle + SettingsView/SubLiftMacApp Picker 接线；swift build/test 全绿。
- [x] **feat-05002 CLI+IPC 接线**：--engine paddle choices + factory 分支；test_cli 更新。
- [x] **feat-05001 PaddleOcrEngine 引擎**：paddle.py 实现 OcrEngine Protocol；rapidocr 依赖接入；模型缓存 ~/.cache/sublift/rapidocr-models。
- [x] **Phase 5.0 规划落成**：plan/phase5.json/feature-list 三产物。

## 阻塞项 / 风险

- [ ] **merged residual / #15 砰**：非 034 主目标，仍开放。
- [ ] **ground truth 素材有限**：性能 baseline 同 Zootopia 片源，不冒充泛化。

## 近期决策

- **Phase 5 PaddleOCR 引擎选型**：rapidocr>=3.9.0 + onnxruntime（PP-OCRv6 small，非 rapidocr-onnxruntime 1.x 停更线）；模型缓存覆盖为 ~/.cache/sublift/rapidocr-models；PaddleOcrEngine 不接 Phase 4.2 归因（契约允许）。见 ADR-0018。

> 完整决策记录见 `docs/DECISIONS.md`
# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-25（Phase 5.0 规划落成，待开始执行）
- **当前 Phase：** phase5-paddle-ocr（规划完成，未开工）
- **当前功能：** feat-05001 PaddleOcrEngine 引擎实现 + 依赖接入（not-started）
- **分支：** feat/ocr-model-support（从 main=7a56ce8 切出，worktree /Volumes/lab/pp/SubLift-ocr）
- **说明：** Phase 5 接入 PaddleOCR 第二引擎（rapidocr>=3.9.0 + onnxruntime，PP-OCRv6 small）。三产物已落成：docs/plans/phase5.md、docs/phases/phase5.json（4 个 feat 全 not-started）、feature-list.json 加 phase5 块。范围仅接入引擎，不接归因、不做路由/对照/跨平台抽象。编号规则变更：Phase 5 起 feat id 用「阶段+序号」feat-05001 起，旧 feat-001~043 不动。

## 进行中

- 规划已落成，待开始执行 feat-05001（pyproject 加 paddle extra + paddle.py 引擎实现）。

## 近期完成（最近 5 个）

- [x] **Phase 5.0 规划落成**：plan/phase5.json/feature-list 三产物；编号规则变更（feat-05001 起）；从 main 重建 feat/ocr-model-support worktree。
- [x] **feat-043 Vision OCR 内部归因**：完成真实 Vision 基线与分流；默认路径不变。
- [x] **feat-043 审查修复与历史整理**：外层对账、untimed 路径；phase4.2 commits 压成 `abafab2`。
- [x] **feat-043 初版实现**：有界归因 / observer / 段 trace / benchmark 对账。
- [x] **docs(phase4)**：ROI 性能优化正式报告。

## 阻塞项 / 风险

- [ ] **merged residual / #15 砰**：非 034 主目标，仍开放。
- [ ] **ground truth 素材有限**：性能 baseline 同 Zootopia 片源，不冒充泛化。
- [ ] **显式 CJK 混排风险**：边界 cleanup 可能误删无空格英文；默认 auto。
- [ ] **summary 不能作产品速度基线**：1.319 > 1.05；根因未被本轮分块三次测量证明。仅在未来需要以 summary 承诺速度时，做交错 off/summary 配对复测。

## 近期决策

- **Phase 5 PaddleOCR 引擎选型**：rapidocr>=3.9.0 + onnxruntime（PP-OCRv6 small，非 rapidocr-onnxruntime 1.x 停更线）；模型缓存覆盖为 ~/.cache/sublift/rapidocr-models；PaddleOcrEngine 不接 Phase 4.2 归因（契约允许）。见 docs/plans/phase5.md。
- **编号规则变更**：Phase 5 起 feat id 采用「阶段+序号」编码 feat-05001 起（05=phase，0=小阶段 5.0，001=序号）；Phase 1-4 旧编号 feat-001~043 保持不动。
- **Vision 请求执行 ≈99% OCR parent**：不做桥接、映射、并行或默认几何微优化；先补多源 GT，再研究代表帧排序与有效调用数。

> 完整决策记录见 `docs/DECISIONS.md`
