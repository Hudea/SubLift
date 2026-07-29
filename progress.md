# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-29（Phase 6 **Phase 6 Native C++ Core Migration 全部开发与硬化完成**）
- **当前 Phase：** phase6-native-cpp-core（**已全部完成**）
- **子阶段：** **6.7 PaddleOCR C++ adapter**（feat-06701–06706 全部完成）
- **6.0–6.7：** 全部 Phase 6 功能开发完成；全引擎（vision / mock / paddle）支持 Native C++ Worker
- **产品默认 Runtime：** C++ `sublift_worker`（vision / mock / paddle 可用时走 C++，不可用时降级 `paddle_override`）
- **回滚：** `SUBLIFT_RUNTIME=python`
- **分支：** 以当前工作区为准（GUI 已优先 `build/cpp-rel` Release worker）

## 进行中

- 无（Phase 6 已完成，准备收口）

## 近期完成（最近 5 个）

- [x] **feat-06706**：Parity harness + paddle 产品默认路由（dump_paddle.py --check, Catch2 paddle_parity_test 26 断言, 11 Parity Goldens 门禁全绿）
- [x] **feat-06705**：Worker / CLI / Runtime 接线与可用性感知路由（C++ EngineFactory bind, python & Swift 路由解封, 严禁引擎篡改）
- [x] **feat-06704**：模型规格 + 缓存路径 + availability 语义（tiny/small/medium 校验, ~ 展开与优先级, Catch2 57 断言全绿）
- [x] **feat-06703**：PaddleOcrEngine::recognize 主路径与 CTC 解码（ppocr_ctc.hpp, 捕获 Ort::Exception 抛出的故障语义，Catch2 46 断言全绿）
- [x] **feat-06702**：CMake + ONNX Runtime 发现 + sublift_paddle 壳（is_paddle_available, stub 抛出 runtime_error, Catch2 跑通）

## 阻塞项 / 风险

- [x] **6.7 Paddle C++**：已完成原生支持 (`sublift_paddle`)
- [ ] **GT L3 live** / ADR-0022 豁免策略
- [ ] **macOS sanitizer + OpenCV/TBB** 退出 134（发布门）
- [ ] **merged residual / #15**
- [ ] **ORT 安装源与 rapidocr 模型布局** 实现时需钉版本（见 phase6.7 §3/§9）

## 近期决策

- **ADR-0023**：6.7 采用 ONNX Runtime + PP-OCRv6；`sublift_paddle` option OFF 默认；可用性感知路由；禁静默改引擎
- **ADR-0022**：缺固定 GT 时 cutover L3 WAIVE
- **6.6 默认 cpp**（vision/mock）；Release 优先 `cpp-rel`

> 完整决策记录见 `docs/DECISIONS.md`
