# feat-06904 可执行计划：薄 Native CLI 链接面

- **Feature ID**: feat-06904
- **Feature Name**: 薄 Native CLI 链接面
- **Phase**: Phase 6.9 (Native 产品架构整理)
- **Worktree**: `/Volumes/lab/pp/SubLift_CPP`
- **关联设计**: `docs/cpp/phase6.9-native-product-architecture.md` / `docs/cpp/phase6.9-implementation-plan.md`

---

## 1. 现状与目标 Diff

### 1.1 现状 (Current State)
1. **链接面过厚，隐式拉入巨石依赖**: `sublift_cli` 在 `cpp/src/cli/CMakeLists.txt` 中链接了巨石 Target `sublift_ipc` (`target_link_libraries(sublift_cli PRIVATE sublift_ipc)`)。由于 `sublift_ipc` 隐式 PUBLIC 传递依赖了 `sublift_application`、`sublift_paddle`、`sublift_vision_macos`、`sublift_ffmpeg`、`OpenCV` 与 Apple `Vision.framework`，导致仅负责 UDS 发包/进程启动的薄 CLI 被迫绑定了全套 heavy 运行时。
2. **ONNX Runtime 显式打包绑定**: `cpp/src/cli/CMakeLists.txt` 中显式调用了 `sublift_use_bundled_onnxruntime(sublift_cli)`，使得 ONNX Runtime 动态库被强制绑定到 CLI 产物。
3. **动态库依赖污染**: 运行 `otool -L build/cpp/bin/sublift_cli` 时，产物直接依赖 `libonnxruntime`、OpenCV 以及 macOS Vision framework 等内部 Worker 引擎层重型库。

### 1.2 目标 (Target State)
1. **极薄 Native CLI 链接面**:
   - 重构 `cpp/src/cli/CMakeLists.txt`，使 `sublift_cli` 仅 PRIVATE 链接 `sublift_protocol` 和 `sublift_core`。
   - 彻底移除对 `sublift_ipc` 的链接以及 `sublift_use_bundled_onnxruntime(sublift_cli)` 的调用。
2. **物理依赖剥离与 `otool -L` 验证**:
   - `build/cpp/bin/sublift_cli` 不再直接或间接链接 `libonnxruntime`、`OpenCV` dylib 或 `Vision.framework`。
3. **架构与通信模式零修改**:
   - CLI 保持 spawn `sublift_worker` 进程并通过 UDS Socket 进行 protocol/framing 通信的模式不变。
   - 零 in-process OCR 逻辑，保持进程间隔离。
   - 保持 CLI 参数解析 (`ExtractArgs`) 与 SRT 输出格式 100% 兼容。

---

## 2. 有序实施步骤（每步可验证）

### Step 1: 重构 `cpp/src/cli/CMakeLists.txt`
- **操作**:
  - 修改 `cpp/src/cli/CMakeLists.txt`：
    1. 将 `target_link_libraries(sublift_cli PRIVATE sublift_ipc)` 替换为：
       `target_link_libraries(sublift_cli PRIVATE sublift_protocol sublift_core)`
    2. 移除以下 ONNXRuntime 绑定代码块：
       ```cmake
       if(COMMAND sublift_use_bundled_onnxruntime)
         sublift_use_bundled_onnxruntime(sublift_cli)
       endif()
       ```
- **验证**:
  - 运行 `cmake -B build/cpp cpp` 确认 CMake 配置正常。
  - 运行 `cmake --build build/cpp --target sublift_cli` 顺利编译并生成 `build/cpp/bin/sublift_cli` 及 `sublift` 别名。

### Step 2: 动态链接库审计 (`otool -L`)
- **操作**:
  - 运行 `otool -L build/cpp/bin/sublift_cli` 审计 CLI 编译产物的动态链接库。
- **验证**:
  - 检查输出，确认 **不** 包含 `libonnxruntime`、`OpenCV` (或 `libopencv_*`) 及 Apple `Vision.framework`。

### Step 3: CLI extract Worker UDS IPC Smoke 验证
- **操作**:
  - 构建 CLI 与 Worker 产物：`cmake --build build/cpp`
  - 创建测试伪视频文件：`touch /tmp/test_cli_smoke.mp4`
  - 运行 mock 提取命令：
    `./build/cpp/bin/sublift_cli extract /tmp/test_cli_smoke.mp4 --engine mock -o /tmp/cli_smoke_out.srt`
  - 清理测试产物：`rm -f /tmp/test_cli_smoke.mp4 /tmp/cli_smoke_out.srt`
- **验证**:
  - CLI 成功 spawn sibling `sublift_worker`，建立 UDS 连接完成 mock extract，顺利生成 SRT 文件，退出码 0。

### Step 4: Catch2 单元测试与 Baseline 全门例行检查 (`./init.sh`)
- **操作**:
  - 执行 Catch2 单元测试套件：`ctest --test-dir build/cpp`
  - 运行日常全门验证路径：`./init.sh`
- **验证**:
  - Catch2 单元测试 100% 通过。
  - `./init.sh` 顺利通过，ruff, mypy, pytest (`not integration`), C++ Debug build, ctest 与 parity 评估检查全绿，且无 golden 变更。

---

## 3. 要改 / 新建的文件列表

### 修改文件
- `cpp/src/cli/CMakeLists.txt` (收缩 PRIVATE 链接为 `sublift_protocol` + `sublift_core`；移除 `sublift_ipc` 链接与 `sublift_use_bundled_onnxruntime` 调用)

### 新建文件
- `docs/cpp/plans/feat-06904-plan.md` (本计划文档)

---

## 4. 测试计划与验收命令

1. **动态库符号与链接审计**:
   `otool -L build/cpp/bin/sublift_cli | grep -E 'onnxruntime|opencv|Vision'` (预期输出为空)
2. **CLI Mock Smoke 测试**:
   `touch /tmp/cli_smoke.mp4 && ./build/cpp/bin/sublift_cli extract /tmp/cli_smoke.mp4 --engine mock -o /tmp/cli_smoke_out.srt && rm -f /tmp/cli_smoke.mp4 /tmp/cli_smoke.mp4 /tmp/cli_smoke_out.srt`
3. **C++ 全量 Catch2 单元测试**:
   `ctest --test-dir build/cpp`
4. **日常全门验证 (Baseline Gate)**:
   `./init.sh`

---

## 5. 风险与回滚

- **风险 1**: `main.cpp` 在剥离 `sublift_ipc` 链接后，因潜在缺少依赖或头文件包含导致编译错误。
  - **规避**: `main.cpp` 仅包含 `framing.hpp`, `protocol.hpp`, `sublift/version.hpp`；`sublift_protocol` 已 PUBLIC 链接 `nlohmann_json` 与 `sublift_core`，依赖干净完备。
- **风险 2**: CLI 运行时依赖 `sublift_worker`，路径探查失败。
  - **规避**: `main.cpp` 中 `resolve_worker()` 已包含 `SUBLIFT_WORKER_PATH` 环境变量、`--worker` 命令行参数及可执行文件同级目录 (sibling) 探查逻辑，保持不变。
- **回滚方案**: 若产生无法解决的链接或构建异常，执行 `git checkout -- cpp/src/cli/CMakeLists.txt` 恢复工作区。

---

## 6. 非目标与约束再确认 (Non-Goals)

1. **不为** CLI 增加 in-process OCR 引擎捷径（严禁直连 OpenCV/Paddle/Vision）。
2. **不修改** CLI 的命令行参数解析逻辑 (`ExtractArgs`) 与 SRT 输出格式。
3. **不改变** CLI 架构与通信模式（必须继续 spawn Worker 进程，通过 UDS socket 通信）。

---

## 7. Done 定义

- [ ] `cpp/src/cli/CMakeLists.txt` 更新完成，`sublift_cli` 仅 PRIVATE 链接 `sublift_protocol` 和 `sublift_core`。
- [ ] 移除 `sublift_ipc` 链接和 `sublift_use_bundled_onnxruntime(sublift_cli)` 调用。
- [ ] `otool -L build/cpp/bin/sublift_cli` 验证无 `libonnxruntime`、`OpenCV` dylib 及 `Vision.framework`。
- [ ] `sublift extract --engine mock` 验证 CLI 仍经 Worker UDS IPC 运行正常并成功输出结果。
- [ ] `ctest --test-dir build/cpp` 执行且全绿。
- [ ] `./init.sh` 全门例行检查全绿，零错误且无 golden 变更。
