# feat-06907 可执行计划：sublift_models Target 与模型布局

- **Feature ID**: feat-06907
- **Feature Name**: sublift_models Target 与模型布局
- **Phase**: Phase 6.9 (Native 产品架构整理)
- **Worktree**: `/Volumes/lab/pp/SubLift_CPP`
- **关联设计**: `docs/cpp/phase6.9-native-product-architecture.md` / `docs/cpp/phase6.9-implementation-plan.md`

---

## 1. 现状与目标 Diff

### 1.1 现状 (Current State)
1. **模型解析逻辑绑定于 Adapter 内部**:
   - 目前 `parse_model_type`, `model_type_to_string`, `expand_user_path`, `resolve_model_dir`, `get_expected_model_paths`, `validate_model_paths` 及 `ModelPaths` 声明于 `cpp/src/adapters/paddle/paddle_models.hpp`，内联实现位于 `cpp/src/adapters/paddle/paddle_models.cpp`，命名空间属于 `sublift::paddle_detail`。
   - 没有独立的 C++ 模型 Target。模型路径解析与校验耦合在 Paddle Adapter 中，其它 Native 组件（如 Worker ResourceLocator / probe / CLI）无法直接引用轻量的模型包定义。
2. **Header 布局缺失通用模型入口**:
   - `cpp/include/sublift/` 下缺乏通用 `models` 模块头文件 `cpp/include/sublift/models/model_bundle.hpp` 以及根转发头 `cpp/include/sublift/model_bundle.hpp`。

### 1.2 目标 (Target State)
1. **创建 `sublift_models` STATIC Target**:
   - 在 `cpp/src/models/` 创建 `CMakeLists.txt` 与 `model_bundle.cpp`。
   - 建立独立静态库 `sublift_models` (ALIAS `sublift::models`)，包含模型解析与路径校验核心实现。
2. **导出标准化 Header**:
   - 在 `cpp/include/sublift/models/model_bundle.hpp` 声明 `ModelType`、`ModelPaths` (及 `ModelBundle` 结构体/类型别名)、`parse_model_type`、`model_type_to_string`、`expand_user_path`、`resolve_model_dir`、`get_expected_model_paths`、`validate_model_paths` 等 API（作用于 `sublift::models` 命名空间）。
   - 提供 `cpp/include/sublift/model_bundle.hpp` 根转发头（内容 `#pragma once\n#include <sublift/models/model_bundle.hpp>`）。
3. **委托与零破坏兼容**:
   - 让 `sublift_paddle` 链接 `sublift_models`。
   - 将 `cpp/src/adapters/paddle/paddle_models.hpp` 改造为无缝兼容转发头：`sublift::paddle_detail` 别名/内联转发到 `sublift::models`，删除原 `paddle_models.cpp` 文件。
   - `paddle_models_test.cpp` 零断言退化（0 assertion regression）。
4. **CMake 级联更新**:
   - 在 `cpp/CMakeLists.txt` 中添加 `add_subdirectory(src/models)`。
   - 在 `cpp/tests/CMakeLists.txt` 中更新 `sublift_tests` 链接 `sublift_models`。
5. **绝对约束与非目标**:
   - **禁止改动**: RapidOCR ONNX 模型节点权重或结构、模型路径解析优先级（`custom_dir` > `SUBLIFT_PADDLE_MODEL_DIR` > `~/.cache/sublift/rapidocr-models`）、RapidOCR 缓存结构与错误提示字符串 (`"缺少 Paddle 模型文件: ..."`）。

---

## 2. 有序实施步骤（每步可验证）

### Step 1: 创建公共 API 头文件
- **操作**:
  - 创建 `cpp/include/sublift/models/model_bundle.hpp`：
    - 在 `sublift::models` 命名空间下定义 `enum class ModelType { Tiny, Small, Medium };`
    - 定义 `struct ModelPaths { ... };` 及 `using ModelBundle = ModelPaths;`
    - 声明 `parse_model_type`, `model_type_to_string`, `expand_user_path`, `resolve_model_dir`, `get_expected_model_paths`, `validate_model_paths`。
  - 创建根转发头 `cpp/include/sublift/model_bundle.hpp`：
    ```cpp
    #pragma once
    #include <sublift/models/model_bundle.hpp>
    ```
- **验证**:
  - 静态代码检查通过，包含路径解析无歧义。

### Step 2: 实现 `sublift_models` STATIC Target
- **操作**:
  - 创建 `cpp/src/models/CMakeLists.txt`：
    ```cmake
    add_library(sublift_models STATIC model_bundle.cpp)
    add_library(sublift::models ALIAS sublift_models)

    target_include_directories(sublift_models
      PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    )

    target_link_libraries(sublift_models PUBLIC sublift_core)
    ```
  - 创建 `cpp/src/models/model_bundle.cpp`：
    - 将 `sublift::paddle_detail` 的模型逻辑迁移到 `sublift::models` 命名空间下实现。
    - 保持错误提示字符串 `"缺少 Paddle 模型文件: "` 及缺失文件拼接规则 100% 相同。
- **验证**:
  - 确认源文件实现完整，代码遵循 Clean Code / C++20 规范。

### Step 3: 重构 `sublift_paddle` 适配器委托
- **操作**:
  - 修改 `cpp/src/adapters/paddle/paddle_models.hpp`：
    - 包含 `<sublift/models/model_bundle.hpp>`。
    - 在 `sublift::paddle_detail` 命名空间中加入 `using ModelType = sublift::models::ModelType;`, `using ModelPaths = sublift::models::ModelPaths;`, `using ModelBundle = sublift::models::ModelBundle;`。
    - 提供内联转发函数：`parse_model_type`, `model_type_to_string`, `expand_user_path`, `resolve_model_dir`, `get_expected_model_paths`, `validate_model_paths` 均直接委托给 `sublift::models::*`。
  - 删除旧实现文件 `cpp/src/adapters/paddle/paddle_models.cpp` (`git rm cpp/src/adapters/paddle/paddle_models.cpp`)。
  - 修改 `cpp/src/adapters/paddle/CMakeLists.txt`：
    - 从 `sublift_paddle` 源文件列表中移除 `paddle_models.cpp`。
    - 添加 `target_link_libraries(sublift_paddle PUBLIC sublift_models)`。
- **验证**:
  - 确认 `sublift_paddle` 编译无缝，依赖 `sublift::paddle_detail` 的现有 Adapter 代码零改动通过。

### Step 4: 更新 CMake 根配置与 Test Target 链接
- **操作**:
  - 更新 `cpp/CMakeLists.txt`：在 `add_subdirectory(src/adapters/paddle)` 之前加入 `add_subdirectory(src/models)`。
  - 更新 `cpp/tests/CMakeLists.txt`：在 `target_link_libraries(sublift_tests PRIVATE ...)` 中加入 `sublift_models`。
  - 新增 `cpp/tests/sublift_models_test.cpp` 覆盖 `sublift::models` 命名空间直调单测，并在 `cpp/tests/CMakeLists.txt` 中注册。
- **验证**:
  - CMake 配置成功生成，Build 依赖图包含 `sublift_models`。

### Step 5: CTest 专项验证
- **操作**:
  - 运行 `ctest --test-dir build/cpp -R 'models|Paddle Models'`
- **验证**:
  - 运行 `paddle_models_test.cpp` 以及新增的 `sublift_models_test.cpp`，100% 绿。

### Step 6: 全日常门校验 (`./init.sh`)
- **操作**:
  - 运行 `./init.sh`
- **验证**:
  - `ruff` / `mypy` / `ctest` / `pytest` 及 cutover parity 检查全绿。

---

## 3. 要改 / 新建 / 移动的文件列表

### 新建文件
- `cpp/include/sublift/models/model_bundle.hpp`
- `cpp/include/sublift/model_bundle.hpp` (根转发头)
- `cpp/src/models/CMakeLists.txt`
- `cpp/src/models/model_bundle.cpp`
- `cpp/tests/sublift_models_test.cpp`
- `docs/cpp/plans/feat-06907-plan.md` (本计划文档)

### 修改文件
- `cpp/CMakeLists.txt`
- `cpp/src/adapters/paddle/CMakeLists.txt`
- `cpp/src/adapters/paddle/paddle_models.hpp`
- `cpp/tests/CMakeLists.txt`

### 删除文件 (`git rm`)
- `cpp/src/adapters/paddle/paddle_models.cpp`

---

## 4. 测试计划与验收命令

1. **Catch2 Models 专项测试**:
   `ctest --test-dir build/cpp -R 'models|Paddle Models'`
2. **日常全门验证 (Baseline Gate)**:
   `./init.sh`

---

## 5. 风险与回滚

- **风险**: `sublift::paddle_detail` 的转发如果缺失部分符号或重载，会导致 `paddle_trace_main.cpp` 或 `paddle_ocr.cpp` 编译失败。
- **规避**: 在 `paddle_models.hpp` 中提供 100% 完整的 inline 转发与 `using` 别名，并在改动后立即执行 CTest 验证。
- **回滚方案**: `git reset --hard HEAD`

---

## 6. 非目标与约束再确认 (Non-Goals)

1. **不修改** RapidOCR ONNX 模型节点权重或结构。
2. **不修改** 默认模型缓存目录与环境变量名称 (`SUBLIFT_PADDLE_MODEL_DIR` 及 `~/.cache/sublift/rapidocr-models`)。
3. **严禁改动** 模型路径解析优先级与 RapidOCR 缓存结构、错误提示字符串。

---

## 7. Done 定义

- [ ] `sublift_models` STATIC target 创建成功 (`cpp/src/models/CMakeLists.txt`, `model_bundle.cpp`)。
- [ ] `cpp/include/sublift/models/model_bundle.hpp` 声明完整 API，`cpp/include/sublift/model_bundle.hpp` 提供无缝转发头。
- [ ] `sublift_paddle` 成功链接 `sublift_models` 并通过 `sublift::paddle_detail` 完成逻辑委托。
- [ ] `cpp/CMakeLists.txt` 中添加 `add_subdirectory(src/models)`，`sublift_tests` 链接 `sublift_models`。
- [ ] `paddle_models_test.cpp` 零断言退化，`sublift_models_test.cpp` 100% 跑通。
- [ ] `ctest --test-dir build/cpp -R 'models|Paddle Models'` 100% 绿。
- [ ] `./init.sh` 验证全绿。
