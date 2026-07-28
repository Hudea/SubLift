# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-29（golden/init/worker 冗余清理完成）
- **当前 Phase：** phase6-native-cpp-core（6.0–6.6 开发完成；合入前卫生债已压到发版门）
- **子阶段 6.0–6.6：** 产品默认 C++；`golden_registry` 单一列表 + cutover 唯一正确性入口；`resolve_worker_bin`（cpp-rel 优先）。
- **产品默认 Runtime：** **C++ Native Core (`sublift_worker`)**（vision / mock 引擎默认；paddle 仍走 Python）
- **回滚机制：** 支持 `SUBLIFT_RUNTIME=python` 100% 干净回滚 Python Runtime
- **分支：** `refactor/cpp`

## 进行中

- 无；下一项应处理 release gate（GT L3 固定资产与 macOS sanitizer/OpenCV-TBB 兼容性）。

## 近期完成（最近 5 个）

- [x] **冗余清理**：`golden_registry` 单一列表；删除 `check_all_goldens`/`--skip-parity`；init 一次 cutover；`resolve_worker_bin`（cpp-rel 优先）；CLI 完成条数=滤空后
- [x] **init.sh 加速与门禁收口**：cutover 单进程 10 golden；可选 `SUBLIFT_INIT_SKIP_*`
- [x] **P1–P5 完整修复与复审**：P1(Vision头文件include)、P2(sys.path路径注入)、P3(Release性能澄清+Worker自动选择)、P4(SRT空字幕双端过滤)、P5(macOS init Vision矩阵)
- [x] **feat-06607** Sanitizer 依赖隔离（无 OpenCV 诊断 Worker + ASan 对照；发布门未豁免）
- [x] **feat-06606** C++ Worker hardening（可移植 E2E、bye→EOF、frame 输入防护、capability、init/文档对齐）
- [x] **Phase 6 C++ 迁移质量审查 + 修复回填**：`docs/reports/phase6-cpp-migration-quality-audit-2026-07-28.md`
- [x] **feat-06605** 默认翻转 + 回滚演练 + release note

## 阻塞项 / 风险

- [ ] **merged residual / #15**
- [ ] **GT L3 live**：固定 clip 不入库；门禁在无 `debug/Zootopia_clip_1080p.mp4` 时按 **ADR-0022** 豁免；有资产机器应用 `--require-gt` 实测
- [ ] **Paddle cutover**：仍 Python worker；UI 须诚实
- [ ] **macOS sanitizer release gate**：`SUBLIFT_SANITIZE=ON` 构建在 Homebrew OpenCV 4.14 / TBB 2023.1.0 上任何链接 OpenCV 的进程退出时为 134（`tbb::detail::r1::__TBB_InitOnce` 析构）；06607 的无 OpenCV ASan Worker 可正常退出，故问题已收窄到 OpenCV/TBB 依赖退出链，但尚未作为内存安全通过。正常 Debug/Release 基线不受影响，须在发布 runner 解决并跑绿。

## 近期决策

- **ADR-0022**：缺固定 GT 视频时 cutover 门 WAIVE live L3，禁止写成全契约 PASS
- **6.6 默认 cpp**（vision/mock）；原生 `build/cpp/bin/sublift extract`；`SUBLIFT_RUNTIME=python` 回滚
- **06606 不扩大架构**：保留 `WorkerConnection → BridgeHandler`；先用协议/资源边界测试收口，显式 JobSession 状态机留待独立需求。

> 完整决策记录见 `docs/DECISIONS.md`
