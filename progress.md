# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-28（Phase 6 Native C++ Core 架构迁移全部完成 🎉）
- **当前 Phase：** phase6-native-cpp-core (**ALL DONE**)
- **子阶段 6.0–6.6：** **100% DONE**（全量 28 个 features 全部完成，门禁 21/21 绿）
- **产品默认 Runtime：** **C++ Native Core (`sublift_worker`)**（vision / mock 引擎默认；paddle 仍走 Python）
- **回滚机制：** 支持 `SUBLIFT_RUNTIME=python` 100% 干净回滚 Python Runtime
- **分支：** `refactor/cpp`

## 进行中

- 无（Phase 6 全部 28 个功能点已完全交付并通过全量验证）

## 近期完成（最近 5 个）

- [x] **feat-06605** 默认翻转 + 回滚演练 + release note (default runtime cpp 翻转 + PipelineClient + README + Phase 6 收官)
- [x] **feat-06604** Cutover 门：正确性 + 运行时 (scripts/parity/check_cutover_gate.py + init.sh 21/21 + test_cutover_gate.py)
- [x] **feat-06603** CLI cutover 路径 (cli.py --runtime 参数 + _run_extract_cpp UDS IPC + TestCliCutoverRouting)
- [x] **feat-06602** macOS GUI 双 Worker 启动 (PipelineClient C++ sublift_worker 启动 + findWorkerExecutable + SO_NOSIGPIPE + XCTest)
- [x] **feat-06601** Runtime 解析策略 + 开关面 (Python runtime.py + Swift RuntimePolicy.swift + 1:1 双端表驱动测试)

## 阻塞项 / 风险

- [ ] **merged residual / #15**
- [ ] **GT L3 live**：固定 clip 不入库；门禁在无 `debug/Zootopia_clip_1080p.mp4` 时按 **ADR-0022** 豁免；有资产机器应用 `--require-gt` 实测
- [ ] **Paddle cutover**：仍 Python worker；UI 须诚实

## 近期决策

- **ADR-0022**：缺固定 GT 视频时 cutover 门 WAIVE live L3，禁止写成全契约 PASS
- **6.6 默认 cpp**（vision/mock）；原生 `build/cpp/bin/sublift extract`；`SUBLIFT_RUNTIME=python` 回滚
- **paddle 永不静默**落到 vision/mock

> 完整决策记录见 `docs/DECISIONS.md`
