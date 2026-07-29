# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-29（6.7 审核修复：recognize 真推理 + 路由接线）
- **当前 Phase：** phase6-native-cpp-core
- **子阶段：** **6.7 PaddleOCR C++ adapter**（审核循环后实现收口）
- **产品默认 Runtime：** C++ `sublift_worker`（vision/mock；paddle 在模型+worker 可用时走 C++）
- **回滚：** `SUBLIFT_RUNTIME=python` / `SUBLIFT_CPP_PADDLE=0`
- **验证烟测：** `uv run sublift extract … --engine paddle --runtime cpp` → 2min clip **43 条**非空 SRT（Release + ORT + PP-OCRv6）

## 进行中

- 无

## 近期完成（最近 5 个）

- [x] **init.sh 瘦身**：单次 pytest、parity 默认、runtime/GT 可选、报告写 /tmp；AGENTS 防膨胀规则
- [x] **审核修复**：ORT recognize + 路由 probe + PP-OCRv6 路径
- [x] **feat-06701–06706** paddle adapter（Det 简化连通域，文本 L4）
- [x] **GUI worker** 优先 `cpp-rel` Release
- [x] **ADR-0023** 选型冻结

## 阻塞项 / 风险

- [ ] **Det 后处理** 非完整 DB unclip → 与 rapidocr 框/召回有差（L4）
- [ ] **GT L3 live** / ADR-0022
- [ ] **macOS sanitizer + OpenCV/TBB** 退出 134
- [ ] **merged residual / #15**
- [ ] 需本机 `brew install onnxruntime` + 模型缓存 + `SUBLIFT_ENABLE_PADDLE=ON` 构建

## 近期决策

- **ADR-0023**：6.7 采用 ONNX Runtime + PP-OCRv6；`sublift_paddle` option OFF 默认；可用性感知路由；禁静默改引擎
- **ADR-0022**：缺固定 GT 时 cutover L3 WAIVE
- **6.6 默认 cpp**（vision/mock）；Release 优先 `cpp-rel`

> 完整决策记录见 `docs/DECISIONS.md`
