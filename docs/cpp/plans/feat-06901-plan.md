# feat-06901 可执行计划：sublift_protocol Target 与协议纯度

- **Feature ID**: feat-06901
- **Feature Name**: sublift_protocol Target 与协议纯度
- **Phase**: Phase 6.9 (Native 产品架构整理)
- **Worktree**: `/Volumes/lab/pp/SubLift_CPP`
- **关联设计**: `docs/cpp/phase6.9-native-product-architecture.md` / `docs/cpp/phase6.9-implementation-plan.md`

---

## 1. 现状与目标 Diff

### 1.1 现状 (Current State)
1. **巨石 `sublift_ipc` 混杂**: `framing.cpp`, `framing.hpp`, `protocol.cpp`, `protocol.hpp` 直接包含在 `sublift_ipc` 的 `_sublift_ipc_sources` 中。
2. **重度污染依赖**: `sublift_ipc` PUBLIC 链接了 `sublift_core`, `sublift_ffmpeg`, `sublift_vision_macos`, `sublift_paddle`, `nlohmann_json`，导致仅仅解析协议或处理 framing 字节流时就引入了 OpenCV、Vision、Paddle 与 ORT 等依赖。
3. **协议探测不纯**: `protocol.cpp` 中硬编码包含了 `<sublift/vision.hpp>`，并在 `build_bye_message()` 中直接调用 `sublift::is_vision_available()`，破坏了 IPC 协议 DTO 与底层 OCR 引擎实现解耦的纯洁性。
4. **单测耦合**: `cpp/tests/worker_protocol_test.cpp` 包含 `<sublift/vision.hpp>` 并直接依赖 `is_vision_available()`。

### 1.2 目标 (Target State)
1. **独立 STATIC Target**: 在 `cpp/src/worker/CMakeLists.txt` 中新增 `sublift_protocol` STATIC 静态库，仅包含 `framing.cpp`, `framing.hpp`, `protocol.cpp`, `protocol.hpp`。
2. **纯粹依赖链**: `sublift_protocol` PUBLIC 仅链接 `sublift_core` 与 `nlohmann_json::nlohmann_json`。绝不链接 Vision, OpenCV, Paddle, ORT, FFmpeg。
3. **`sublift_ipc` 链接**: `sublift_ipc` 移除 `framing.*` 和 `protocol.*` 源文件，转为 PUBLIC 链接 `sublift_protocol`。
4. **协议纯化与参数注入**:
   - 移除 `protocol.cpp` 中的 `#include <sublift/vision.hpp>` 与 `sublift::is_vision_available()`。
   - 重构 `build_bye_message()` 为可注入参数接口：
     `ByeMsg build_bye_message(std::vector<std::string> engines = {"mock"}, std::vector<std::string> capabilities = {"path_mode", "frame_mode", "push_entry", "cancel"});`
     默认重载保持 `{"mock"}` Baseline，保证 Pure Protocol DTO 诚实性；由 Composition Root (`bridge.cpp` / `EngineFactory`) 传入实际支持的 engines 与 capabilities。
5. **测试解耦**: `cpp/tests/worker_protocol_test.cpp` 移除 `#include "sublift/vision.hpp"`，断言逻辑不再直接调用 `is_vision_available()`。

---

## 2. 有序实施步骤（每步可验证）

### Step 1: 重构 `protocol.hpp` / `protocol.cpp` 实现协议纯化
- **操作**:
  - 在 `protocol.cpp` 中删除 `#include <sublift/vision.hpp>`。
  - 在 `protocol.hpp` 中将 `build_bye_message()` 声明更新为支持默认参数注入：
    ```cpp
    [[nodiscard]] ByeMsg build_bye_message(
        std::vector<std::string> engines = {"mock"},
        std::vector<std::string> capabilities = {"path_mode", "frame_mode", "push_entry", "cancel"});
    ```
  - 在 `protocol.cpp` 中实现 `build_bye_message`，直接使用传入的 `engines` 与 `capabilities` 填充 `ByeMsg` 字段。
- **验证**:
  - 检查 `protocol.cpp` 源码，确认零 Vision/OpenCV/Paddle 头文件与 API 引用。

### Step 2: 修改 `cpp/src/worker/CMakeLists.txt` 构建目标
- **操作**:
  - 声明 `sublift_protocol` STATIC 库，包含 `framing.cpp`, `framing.hpp`, `protocol.cpp`, `protocol.hpp`。
  - 配置 `target_include_directories(sublift_protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})`。
  - 配置 `target_link_libraries(sublift_protocol PUBLIC sublift_core nlohmann_json::nlohmann_json)`。
  - 从 `_sublift_ipc_sources` 中移除 `framing.*` 与 `protocol.*`。
  - 在 `target_link_libraries(sublift_ipc ...)` 中添加 `sublift_protocol`。
- **验证**:
  - 运行 `cmake -B build/cpp cpp` 成功生成 Makefile / Ninja 节点。
  - 运行 `cmake --build build/cpp --target sublift_protocol sublift_ipc` 构建成功。

### Step 3: 更新 `cpp/tests/worker_protocol_test.cpp`
- **操作**:
  - 移除 `#include "sublift/vision.hpp"`。
  - 调整 `EngineFactory` 测试用例中的 vision 引擎可用性断言，通过 `vision_factory.supported_engines()` 返回值是否非空来进行验证，避免直接包含与调用 `sublift::is_vision_available()`。
- **验证**:
  - 编译测试套件：`cmake --build build/cpp --target sublift_tests`。
  - 运行 ctest：`ctest --test-dir build/cpp -R 'worker_framing|worker_protocol'` 确保全绿。

### Step 4: 依赖审计与全门验收
- **操作**:
  - 使用 `otool -L` 或 CMake 依赖图对 `libsublift_protocol.a` 镜像依赖进行检查。
  - 运行 `./init.sh` 确保完整的日常验证门无报错。
- **验证**:
  - `otool -L build/cpp/src/worker/libsublift_protocol.a`（无 Vision / OpenCV / ORT / Paddle 依赖）。
  - `./init.sh` 全门通过。

---

## 3. 要改 / 新建的文件列表

### 修改文件
- `cpp/src/worker/CMakeLists.txt` (声明 sublift_protocol Target 并调整 sublift_ipc 链接)
- `cpp/src/worker/protocol.hpp` (build_bye_message 签名更新与默认参数注入)
- `cpp/src/worker/protocol.cpp` (移除 vision.hpp 引用，重构 build_bye_message)
- `cpp/tests/worker_protocol_test.cpp` (移除 vision.hpp 引用并更新断言)

### 新建文件
- `docs/cpp/plans/feat-06901-plan.md` (本计划文档)

---

## 4. 测试计划

1. **单元测试 (Catch2)**:
   `ctest --test-dir build/cpp -R 'worker_framing|worker_protocol'`
2. **依赖纯净度专项审计**:
   `otool -L build/cpp/src/worker/libsublift_protocol.a`
   (确认输出仅含系统基本库或 sublift_core，绝对无 Vision.framework / OpenCV / ONNXRuntime)
3. **日常门验证 (Baseline Gate)**:
   `./init.sh`
4. **Cutover Parity 检查**:
   `uv run python scripts/parity/check_cutover_gate.py --check --skip-runtime --skip-gt`

---

## 5. 风险与回滚

- **风险 1**: `build_bye_message()` 签名更改导致 `bridge.cpp` 或 `bridge_no_opencv.cpp` 编译不匹配。
  - **规避**: 使用 C++ 默认参数 `engines = {"mock"}`，在不传参时向下兼容纯协议默认响应；在 `bridge` 中视情况使用注入的 `engine_factory_.supported_engines()` / `capabilities()`。
- **风险 2**: 头文件包含路径不完整导致外部 Target 找不到 `protocol.hpp`。
  - **规避**: 在 `sublift_protocol` 中指定 `PUBLIC` 的 `${CMAKE_CURRENT_SOURCE_DIR}` 头文件搜索路径，且 `sublift_ipc` PUBLIC 链接 `sublift_protocol`。
- **回滚方案**: 若验证失败，可执行 `git checkout -- cpp/src/worker/ cpp/tests/` 恢复代码改动。

---

## 6. 非目标再确认 (Non-Goals)

1. **不修改** framing 字节序、Magic number 或 JSON 消息 DTO 结构与 Schema。
2. **不改变** IPC 握手行为与消息交互流程。
3. **拆分 bridge / connection** 属于 `feat-06902` 的工作范围，本任务禁止越界修改。
4. **禁止改动** 算法、Golden 评估数据集或 DTO 字段。

---

## 7. Done 定义

- [ ] `sublift_protocol` STATIC 在 `cpp/src/worker/CMakeLists.txt` 中正确声明。
- [ ] `sublift_protocol` 仅链接 `sublift_core` 与 `nlohmann_json::nlohmann_json`。
- [ ] `sublift_ipc` 成功链接 `sublift_protocol` 且包含源文件解耦。
- [ ] `protocol.cpp` 彻底移除 `#include <sublift/vision.hpp>` 与对 `is_vision_available()` 的直接依赖。
- [ ] `build_bye_message()` 重构为依赖注入模式，默认重载保持纯协议 Baseline (`{"mock"}`)。
- [ ] `cpp/tests/worker_protocol_test.cpp` 移除对 `vision.hpp` 的包含。
- [ ] `ctest -R 'worker_framing|worker_protocol'` 执行通过且全绿。
- [ ] `otool -L build/cpp/src/worker/libsublift_protocol.a` 验证无 Vision/OpenCV/ORT 动态库依赖。
- [ ] `./init.sh` 脚本日常启动与集成验证通过。
