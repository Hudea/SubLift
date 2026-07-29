# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-30（6.8 Paddle Native hardening **feat-06806 完成**）
- **当前 Phase：** phase6-native-cpp-core
- **子阶段：** **6.8 Paddle Native hardening**（feat-06801 ~ feat-06806 完成，最后一刀 feat-06807 Cutover）
- **产品默认 Runtime：** C++ `sublift_worker`（vision/mock）；paddle 在未显式 opt-in 时安全默认走 Python (`product_default`)
- **回滚：** `SUBLIFT_RUNTIME=python` / `SUBLIFT_CPP_PADDLE=0`
- **分支：** `refactor/paddle-native-hardening`

## 进行中

- **feat-06807**（Paddle C++ 产品 Cutover）
- 设计源头：`docs/cpp/phase6.8-paddle-hardening.md`

## 近期完成（最近 5 个）

- [x] **feat-06806**：Paddle Native 性能加固与评估（Wall-time 0.76x / Peak RSS 0.61x 硬门评估与 check_paddle_perf.py）
- [x] **feat-06805**：多源 Paddle E2E 质量门（Timing F1/CER/Usable/Box IoU 5 维硬门评估与 check_paddle_gate.py）
- [x] **feat-06804**：Paddle Quad Crop、Cls 与 Rec Parity（warpPerspective 透视变换剪裁、高窄框 90° 校正、Rec 3x48xW 右侧 0 Padding 与 text_score=0.5 过滤）
- [x] **feat-06803**：Paddle Det 预处理与完整 DB/unclip parity（包含 0.3 threshold 二值化、2x2 dilation、findContours、Polygon Unclip 与空图 0 Rec 调用的性能短路）
- [x] **feat-06802**：Paddle 分阶段观测与冻结 Oracle Fixture（依赖/模型 SHA256 冻结，10 阶段节点 JSON schema，12 Parity goldens）

## 阻塞项 / 风险

- [ ] **当前安全风险**：06801 未实现前，C++ Paddle 可用时仍会被自动选为默认
- [ ] **质量债**：简化 Det/AABB crop/缺 Cls/逐框 Rec/无框整图 fallback，与 RapidOCR 不等价
- [ ] **性能债**：2min 样片 C++ 147.8s vs Python 54.0s（约 2.74×）；ORT 单线程与无 Rec batch 是首要假设
- [ ] **基线门**：`./init.sh` 当前有既存 ruff import-sort 失败（`tests/ipc/test_cpp_worker.py:9`）
- [ ] **GT L3 live** / ADR-0022
- [ ] **macOS sanitizer + OpenCV/TBB** 退出 134
- [ ] **merged residual / #15**
- [ ] 需本机 `brew install onnxruntime` + 模型缓存 + `SUBLIFT_ENABLE_PADDLE=ON` 构建

## 近期决策

- **ADR-0024**：6.8 先做 Paddle 质量/性能 hardening；安全路由先行，质量冻结后优化，去 Python/打包顺延 6.9+
- **ADR-0023**：6.7 采用 ONNX Runtime + PP-OCRv6；`sublift_paddle` option OFF 默认；可用性感知路由；禁静默改引擎
- **ADR-0022**：缺固定 GT 时 cutover L3 WAIVE

> 完整决策记录见 `docs/DECISIONS.md`
