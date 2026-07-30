# feat-06902 可执行计划：sublift_application Target 与 拆解 sublift_ipc

- **Feature ID**: feat-06902
- **Feature Name**: sublift_application + 拆解 sublift_ipc
- **Phase**: Phase 6.9 (Native 产品架构整理)
- **Worktree**: `/Volumes/lab/pp/SubLift_CPP`
- **关联设计**: `docs/cpp/phase6.9-native-product-architecture.md` / `docs/cpp/phase6.9-implementation-plan.md`

---

## 1. 现状与目标 Diff

### 1.1 现状 (Current State)
1. **巨石 `sublift_ipc` 架构**: `sublift_ipc` 目前为一个涵盖 `engine_factory.*`、`bridge.hpp`、`bridge.cpp` / `bridge_no_opencv.cpp`、`connection.*` 的全量巨石库。
2. **PUBLIC 全量 Fan-out**: `sublift_ipc` PUBLIC 链接了 `sublift_protocol`, `sublift_core`, `sublift_ffmpeg`, `sublift_vision_macos`, `sublift_paddle`, `nlohmann_json`，导致上层依赖项无条件继承所有适配器与基础依赖。
3. **职责混合**: Job 生命周期 (`ExtractJob`)、`BridgeHandler` 消息分发逻辑与 Composition Root 装配 (`EngineFactory`) 和 Socket 传输连接 (`UdsConnection`) 紧密交织在同一个 Target 中。

### 1.2 目标 (Target State)
1. **独立 STATIC Target `sublift_application`**:
   - 在 `cpp/src/worker/CMakeLists.txt` 中提取 `bridge.hpp`, `bridge.cpp` / `bridge_no_opencv.cpp` 组装为 `sublift_application` STATIC 静态库。
   - 包含 Job 生命周期 (`ExtractJob`)、`BridgeHandler` 消息路由、进度回调与取消协调。
   - `sublift_application` PUBLIC 链接 `sublift_protocol`, `sublift_core`, `sublift_ffmpeg`, `sublift_vision_macos`, `sublift_paddle`, `nlohmann_json::nlohmann_json`。
2. **Worker 保持 Composition Root**:
   - `engine_factory.hpp/cpp` 与 `connection.hpp/cpp` 保持作为 Worker 层的装配机制。
   - 由 `EngineFactory` / `main` 组装具体 OCR 引擎并注入到 `BridgeHandler` 与 `UdsConnection` 中。
3. **拆解与重构 `sublift_ipc`**:
   - 将 `sublift_ipc` 从全量 fan-out 巨石库转为由 `sublift_application` 与 `sublift_protocol` 构成的模块化 Target 结构。
   - `sublift_worker` 显式链接 `sublift_application` 及相关 Adapters。
4. **零中断与零退化保证**:
   - 保持 `sublift_cli` 和 `sublift_tests` 在 Target 拆解后的构建与链接零中断、零退化。

---

## 2. 有序实施步骤（每步可验证）

### Step 1: 创建 `sublift_application` STATIC Target
- **操作**:
  - 在 `cpp/src/worker/CMakeLists.txt` 中定义 `sublift_application` 静态库，包含 `bridge.hpp` 和 `bridge.cpp` (若开启 OpenCV) 或 `bridge_no_opencv.cpp` (若未开启 OpenCV)。
  - 配置 `target_include_directories(sublift_application PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})`。
  - 配置 `target_link_libraries(sublift_application PUBLIC sublift_protocol sublift_core sublift_ffmpeg sublift_vision_macos sublift_paddle nlohmann_json::nlohmann_json)`。
  - 在 macOS 下 PUBLIC 链接苹果系统 CoreFoundation, CoreGraphics, ImageIO 框架。
  - 设置 `SUBLIFT_HAS_OPENCV` 编译宏。
- **验证**:
  - 运行 `cmake -B build/cpp cpp`
  - 运行 `cmake --build build/cpp --target sublift_application` 确保成功生成 `libsublift_application.a`。

### Step 2: 拆解 `sublift_ipc` 并更新 `sublift_worker` 链接
- **操作**:
  - 将 `sublift_ipc` 源文件收缩为 `engine_factory.cpp`, `engine_factory.hpp`, `connection.cpp`, `connection.hpp`。
  - 让 `sublift_ipc` PUBLIC 链接 `sublift_application` 与 `sublift_protocol`，透传必要的头文件搜索路径与依赖。
  - 在 `sublift_worker` 的 `target_link_libraries` 中显式链接 `sublift_application` 和 `sublift_ipc`（或适配器）。
- **验证**:
  - 运行 `cmake --build build/cpp --target sublift_worker sublift_ipc` 成功完成编译与链接。

### Step 3: 验证 `sublift_cli` 与 `sublift_tests` 零中断构建与单测 pass
- **操作**:
  - 编译 `sublift_cli` 和 `sublift_tests`：
    `cmake --build build/cpp --target sublift_cli sublift_tests`
  - 运行 Worker Catch2 单元/集成测试：
    `ctest --test-dir build/cpp -R 'Worker|worker'`
  - 运行 Python Worker IPC 端到端测试：
    `uv run pytest tests/ipc/test_cpp_worker.py`
- **验证**:
  - Catch2 `Worker` 测试全绿。
  - Pytest `test_cpp_worker.py` 100% 通过。

### Step 4: 全门验收 (`./init.sh`)
- **操作**:
  - 运行项目日常启动门：`./init.sh`
- **验证**:
  - ruff, mypy, pytest (`not integration`), C++ Debug build, Catch2 ctest, cutover parity checks 全通过，且无 golden 变更。

---

## 3. 要改 / 新建的文件列表

### 新建文件
- `docs/cpp/plans/feat-06902-plan.md` (本计划文档)

### 修改文件
- `cpp/src/worker/CMakeLists.txt` (声明 sublift_application Target，拆解 sublift_ipc 源文件与链接关系)

---

## 4. 测试计划与验收命令

1. **Worker Catch2 测试 (Path / Frame mode / Cancel / Disconnect)**:
   `ctest --test-dir build/cpp -R 'Worker|worker'`
2. **Python IPC Worker 测试**:
   `uv run pytest tests/ipc/test_cpp_worker.py`
3. **日常全门验证 (Baseline Gate)**:
   `./init.sh`

---

## 5. 风险与回滚

- **风险 1**: `sublift_cli` 或 `sublift_tests` 在 `sublift_ipc` 拆解后因头文件包含或符号缺失导致编译/链接错误。
  - **规避**: `sublift_ipc` PUBLIC 链接 `sublift_application` 和 `sublift_protocol`，保证符号与包含路径平滑透传，不破坏外部 Consumer 的 Target 假设。
- **风险 2**: OpenCV / non-OpenCV 条件编译在 `sublift_application` 中丢失 `SUBLIFT_HAS_OPENCV` 定义导致符号未定义。
  - **规避**: 在 `sublift_application` 内部显式设置 `PUBLIC` 级别的 `SUBLIFT_HAS_OPENCV=1/0` 编译宏。
- **回滚方案**: 若出现严重构建崩溃，执行 `git checkout -- cpp/src/worker/CMakeLists.txt` 恢复工作区。

---

## 6. 非目标与约束再确认 (Non-Goals)

1. **不改变** path mode / frame mode 业务语义、cancel 时序、IPC 消息 JSON 契约。
2. **不从** `sublift_core` 拆出 `sublift_pipeline`（归属于 feat-06903）。
3. **不收缩** 薄 CLI 链接面（归属于 feat-06904）。
4. **禁止改动** 算法、golden 评估数据集、IPC 协议与 cancellation 时序行为。

---

## 7. Done 定义

- [ ] `sublift_application` STATIC Target 在 `cpp/src/worker/CMakeLists.txt` 中正确声明（包含 `bridge.hpp`, `bridge.cpp` / `bridge_no_opencv.cpp`）。
- [ ] `sublift_application` PUBLIC 链接 `sublift_protocol` + `sublift_core` + `sublift_ffmpeg` + `sublift_vision_macos` + `sublift_paddle` + `nlohmann_json::nlohmann_json`。
- [ ] Worker 进程保持 Composition Root 角色 (`engine_factory.hpp/cpp`, `connection.hpp/cpp`, `main.cpp`)，由 EngineFactory / main 组装引擎并注入 BridgeHandler / connection。
- [ ] 巨石 `sublift_ipc` 从全量 fan-out 库转为由 `sublift_application` 和 `sublift_protocol` 构成的模块化 Target。`sublift_worker` 显式链接 `sublift_application` 及适配器。
- [ ] `sublift_cli` 与 `sublift_tests` 的编译与链接在 Target 拆解后零中断、零退化。
- [ ] `ctest --test-dir build/cpp -R 'Worker|worker'` 100% 通过。
- [ ] `pytest tests/ipc/test_cpp_worker.py` 100% 通过。
- [ ] `./init.sh` 全门例行检查全绿，零退化、零 golden 变更。
