# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-26（Phase 5 PaddleOCR 审查整改）
- **当前 Phase：** phase5-paddle-ocr（已完成，审查整改已验收）
- **当前功能：** 全部 feat-05001 ~ feat-05004 已完成；PaddleOCR 接入审查整改已验收
- **分支：** feat/ocr-model-support（从 main=7a56ce8 切出，worktree /Volumes/lab/pp/SubLift-ocr）
- **说明：** PaddleOCR 第二引擎已接入（rapidocr 3.9.2 + onnxruntime，PP-OCRv6 small）。CLI `--engine paddle`、IPC server、GUI Picker 已接线；审查整改的验证证据以 `docs/phases/phase5.json` 为准。

## 进行中

- 无

## 近期完成（最近 5 个）

- [x] **PaddleOCR 审查整改验收**：修复默认基线会触发模型下载、RGB/BGR 回归缺口、CLI 初始化错误、IPC engine 不一致与 frame-mode 断连；消除 OpenCV 双装、锁定 RapidOCR 3.x，并完成 Python/Swift/显式 integration 复核。验证见 `docs/phases/phase5.json`。
- [x] **feat-05004 测试+文档收尾**：test_ocr.py 加 6 个 PaddleOcrEngine 测试；design/ocr.md + README + REQUIREMENTS F14 ✅ + DECISIONS ADR-0018；init.sh 更新装 vision+paddle extras。
- [x] **feat-05003 GUI 引擎选择**：OcrEngineName case paddle + SettingsView/SubLiftMacApp Picker 接线；swift build/test 全绿。
- [x] **feat-05002 CLI+IPC 接线**：--engine paddle choices + factory 分支；test_cli 更新。
- [x] **feat-05001 PaddleOcrEngine 引擎**：paddle.py 实现 OcrEngine Protocol；rapidocr 依赖接入；模型缓存 ~/.cache/sublift/rapidocr-models。

## 阻塞项 / 风险

- [ ] **merged residual / #15 砰**：非 034 主目标，仍开放。
- [ ] **ground truth 素材有限**：性能 baseline 同 Zootopia 片源，不冒充泛化。
- [ ] **PaddleOCR 首次模型下载**：首次使用需要网络与模型源可用；CLI 已提供错误原因与预下载路径，缓存后可离线复用。

## 近期决策

- **Phase 5 PaddleOCR 引擎选型**：rapidocr>=3.9.0,<4.0.0 + onnxruntime（PP-OCRv6 small，非 rapidocr-onnxruntime 1.x 停更线）；模型缓存覆盖为 ~/.cache/sublift/rapidocr-models；PaddleOcrEngine 不接 Phase 4.2 归因（契约允许）。见 ADR-0018。
- **Paddle CLI 初始化故障语义**：仅在构造 `PaddleOcrEngine` 的外部依赖/模型初始化阶段转换为用户可操作的 CLI 错误；识别运行时错误继续按 IPC/调用边界语义处理。
- **IPC 引擎权威来源**：服务进程 `--engine` 绑定实际 OCR 工厂；`start_job.engine` 不一致时明确失败，frame/finalize 故障保留原文并清理状态。见 ADR-0019。

> 完整决策记录见 `docs/DECISIONS.md`
