# feat-06908 可执行计划：ResourceLocator 真实资源探查与 Probe=Construct 同一化

- **Feature ID**: feat-06908
- **Feature Name**: ResourceLocator 真实资源探查与 Probe=Construct 同一化
- **Phase**: Phase 6.9 (Native 产品架构整理)
- **Worktree**: `/Volumes/lab/pp/SubLift_CPP`
- **关联设计**: `docs/cpp/phase6.9-native-product-architecture.md` / `docs/cpp/phase6.9-implementation-plan.md`

---

## 1. 现状与目标 Diff

### 1.1 现状 (Current State)
1. **资源查找逻辑分散于各适配器与工具模块**:
   - Paddle 模型路径的解析与校验散落在 `sublift::models::resolve_model_dir` 与 `validate_model_paths` 中。
   - FFmpeg 可执行文件与 ONNX Runtime 共享库的查找缺乏统一、强类型的 Native C++ 资源定位器 (`ResourceLocator`) 机制。
2. **Probe 探查与 Construct 构造链未完全同一化**:
   - `EngineFactory::validate_engine` 和 `sublift::is_paddle_available()` 仅做简单的存在性检查，未与 `PaddleOcrEngine` 构造函数共用底层 `ResourceLocator` 查找与解析链。
   - 存在 probe 判为可用，但 construct 时寻找不同路径或因环境不一致报错的隐患（不符合 Probe ≡ Construct 约束）。
3. **缺乏查找层级优先级与强类型结果封装**:
   - 缺少强类型 `ResourceResult<T>` 表达探查状态（成功/资源来源/缺失清单/报错信息）。
   - 未标准化统一下述 3 层优先级：1. 用户显式指定 / 环境变量 -> 2. Native .app Bundle Resources -> 3. 用户 Cache (`~/.cache/sublift/rapidocr-models`) / 系统 PATH。

### 1.2 目标 (Target State)
1. **在 `sublift_models` Target 中实现 `ResourceLocator`**:
   - 新建 `cpp/include/sublift/models/resource_locator.hpp` 与 `cpp/src/models/resource_locator.cpp`，并在根部提供 `cpp/include/sublift/resource_locator.hpp` 转发头。
   - 统一支持 3 类核心资源的查找与校验：Paddle 模型包 (`ModelPaths`)、FFmpeg/ffprobe 可执行文件、ONNX Runtime 共享库。
   - 统一查找路径优先级：1. 显式 Override / 环境变量 (`SUBLIFT_PADDLE_MODEL_DIR`, `SUBLIFT_FFMPEG_PATH`, `SUBLIFT_ORT_LIB_DIR`) -> 2. Native .app Bundle Resources (`<bundle>/Contents/Resources/models`, `<bundle>/Contents/Resources/bin`, `<bundle>/Contents/Frameworks`) -> 3. 用户 Cache (`~/.cache/sublift/rapidocr-models`, 系统 PATH)。
   - 提供强类型 `ResourceResult<T>`（包含 `found`, `value`, `source`, `error_msg`）以及 `probe_model_bundle` / `locate_model_bundle` / `locate_ffmpeg_executable` / `locate_onnxruntime_library` 方法。
2. **Probe 逻辑与 Construct 构造逻辑同一化 (Probe ≡ Construct)**:
   - 重构 `EngineFactory::validate_engine`、`EngineFactory::supported_engines`、`sublift::is_paddle_available()` 与 `PaddleOcrEngine` 构造逻辑。
   - 确保 `is_paddle_available()` 与 `PaddleOcrEngine` 构造均经过相同的 `ResourceLocator::probe_model_bundle` / `locate_model_bundle` 路径解析链，且探查失败时的错误描述完全一致。
3. **补充 Catch2 单元测试覆盖**:
   - 新建 `cpp/tests/resource_locator_test.cpp`，覆盖各查找层级优先级、fail-closed 缺失校验、Probe ≡ Construct 同一性断言。
4. **绝对约束与非目标**:
   - **Out of scope**: 不在网络层自动从 CDN 下载模型；不修改 `.app` Bundle 物理目录结构。
   - **禁止改动**: 资源查找优先级语义、未找到模型时的具体错误提示短语 (`"缺少 Paddle 模型文件: ..."`).

---

## 2. 有序实施步骤（每步可验证）

### Step 1: 设计并实现 `ResourceLocator` 公共 API 头文件
- **操作**:
  - 创建 `cpp/include/sublift/models/resource_locator.hpp`：
    - 在 `sublift::models` 命名空间下定义 `enum class ResourceSource { ExplicitOverride, AppBundle, UserCache, SystemPath };`
    - 定义模板结构体 `template<typename T> struct ResourceResult { bool found{false}; T value{}; ResourceSource source{ResourceSource::UserCache}; std::string error_msg{}; };`
    - 定义 `class ResourceLocator`：
      - `ResourceResult<ModelPaths> probe_model_bundle(const std::string& custom_dir = "", ModelType type = ModelType::Small) const;`
      - `ResourceResult<ModelPaths> locate_model_bundle(const std::string& custom_dir = "", ModelType type = ModelType::Small) const;`
      - `ResourceResult<std::filesystem::path> locate_ffmpeg_executable(const std::string& custom_path = "") const;`
      - `ResourceResult<std::filesystem::path> locate_onnxruntime_library(const std::string& custom_path = "") const;`
  - 创建根转发头 `cpp/include/sublift/resource_locator.hpp`：
    ```cpp
    #pragma once
    #include <sublift/models/resource_locator.hpp>
    ```
- **验证**:
  - 头文件语法校验正确，`#include <sublift/models/resource_locator.hpp>` 无歧义。

### Step 2: 在 `sublift_models` Target 中实现 `ResourceLocator`
- **操作**:
  - 创建 `cpp/src/models/resource_locator.cpp`：
    - 实现按优先级 1. Override / Env -> 2. Native .app Bundle -> 3. User Cache / System 的查找链。
    - Native .app Bundle 探测：通过当前进程执行路径推算 App Bundle 根路径 (`<app>/Contents/Resources/models` 等)。
    - Paddle 模型探查集成 `validate_model_paths` 校验，保持错误短语前缀为 `"缺少 Paddle 模型文件: "`。
  - 修改 `cpp/src/models/CMakeLists.txt`：
    - 将 `resource_locator.cpp` 添加至 `sublift_models` 静态库源文件列表。
- **验证**:
  - 运行 CMake 构建，确认 `sublift_models` Target 编译成功。

### Step 3: 重构 `PaddleOcrEngine` 与 `is_paddle_available()`
- **操作**:
  - 修改 `cpp/src/adapters/paddle/paddle_ocr.cpp`：
    - 让 `is_paddle_available()` 内部调用 `sublift::models::ResourceLocator{}.probe_model_bundle()`。
    - 让 `PaddleOcrEngine` 构造函数调用 `sublift::models::ResourceLocator{}.locate_model_bundle(options.model_root_dir, parse_model_type(options.model_type))` 获取模型路径。若探查失败，抛出包含 `ResourceResult::error_msg` 的 `std::runtime_error`。
- **验证**:
  - 现有 `paddle_stub_test` 与 paddle unit ctest 编译并通过。

### Step 4: 重构 `EngineFactory::validate_engine` 统一 Probe 链
- **操作**:
  - 修改 `cpp/src/worker/engine_factory.cpp`：
    - 在 `validate_engine` 中处理 `"paddle"` 时，直接复用 `ResourceLocator` / `is_paddle_available()`，确保校验提示信息与底层 `locate_model_bundle` 报错严格相符。
- **验证**:
  - 确认 `EngineFactory` 的探查与构造调用的底层解析逻辑完全一致。

### Step 5: 新建 `resource_locator_test.cpp` Catch2 单测
- **操作**:
  - 创建 `cpp/tests/resource_locator_test.cpp`：
    - 测试 Override 环境变量/自定义路径探查。
    - 测试 User Cache 默认路径探查。
    - 测试非法/缺失路径 fail-closed（`found == false` 且错误信息符合预期）。
    - 测试 Probe ≡ Construct 结果一致性。
  - 修改 `cpp/tests/CMakeLists.txt`：
    - 注册 `resource_locator_test` 到 `sublift_tests`。
- **验证**:
  - 运行 `ctest --test-dir build/cpp -R 'resource_locator|EngineFactory'` 100% 通过。

### Step 6: 全日常门校验 (`./init.sh`)
- **操作**:
  - 运行 `./init.sh`
- **验证**:
  - `ruff` / `mypy` / `ctest` / `pytest` 及 cutover parity 检查全绿。

---

## 3. 要改 / 新建 / 移动的文件列表

### 新建文件
- `cpp/include/sublift/models/resource_locator.hpp`
- `cpp/include/sublift/resource_locator.hpp` (根转发头)
- `cpp/src/models/resource_locator.cpp`
- `cpp/tests/resource_locator_test.cpp`
- `docs/cpp/plans/feat-06908-plan.md` (本计划文档)

### 修改文件
- `cpp/src/models/CMakeLists.txt`
- `cpp/src/adapters/paddle/paddle_ocr.cpp`
- `cpp/src/worker/engine_factory.cpp`
- `cpp/tests/CMakeLists.txt`

---

## 4. 测试计划与验收命令

1. **Catch2 资源探查与 EngineFactory 专项测试**:
   `ctest --test-dir build/cpp -R 'resource_locator|EngineFactory'`
2. **日常全门验证 (Baseline Gate)**:
   `./init.sh`

---

## 5. 风险与回滚

- **风险**: 修改 `is_paddle_available` 与 `PaddleOcrEngine` 构造逻辑后，若环境未显式设置 `model_root_dir` 且无 cache，现有部分依赖 stub 的测试可能受到影响。
- **规避**: 在 `ResourceLocator` 中保持对 `resolve_model_dir` 与默认 `~/.cache/sublift/rapidocr-models` 的 100% 兼容。
- **回滚方案**: `git reset --hard HEAD`

---

## 6. 非目标与约束再确认 (Non-Goals)

1. **不包含** 从 CDN 自动下载模型文件的网络下载逻辑（保留 Python / 外部脚本准备）。
2. **不修改** `.app` Bundle 物理目录结构（属 feat-06909/06910）。
3. **严禁改动** 资源查找优先级语义（Override -> App Bundle -> User Cache）与缺失模型时的具体错误提示短语（`"缺少 Paddle 模型文件: ..."`）。

---

## 7. Done 定义

- [ ] `ResourceLocator` 在 `sublift_models` Target 中实现完整 (`resource_locator.hpp`, `resource_locator.cpp`)。
- [ ] 提供根转发头 `cpp/include/sublift/resource_locator.hpp`。
- [ ] 探查优先级严格满足：1. Override/Env -> 2. App Bundle -> 3. User Cache。
- [ ] `EngineFactory::validate_engine` 与 `PaddleOcrEngine` 构造统一使用 `ResourceLocator` 解析链。
- [ ] `resource_locator_test.cpp` 增加 Catch2 覆盖并 100% 通过。
- [ ] `ctest -R 'resource_locator|EngineFactory'` 100% 绿。
- [ ] `./init.sh` 全日常门 10/10 绿。
