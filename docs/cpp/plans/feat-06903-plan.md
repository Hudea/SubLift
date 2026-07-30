# feat-06903 可执行计划：sublift_pipeline 从 core 拆出

- **Feature ID**: feat-06903
- **Feature Name**: sublift_pipeline 从 core 拆出
- **Phase**: Phase 6.9 (Native 产品架构整理)
- **Worktree**: `/Volumes/lab/pp/SubLift_CPP`
- **关联设计**: `docs/cpp/phase6.9-native-product-architecture.md` / `docs/cpp/phase6.9-implementation-plan.md`

---

## 1. 现状与目标 Diff

### 1.1 现状 (Current State)
1. **`sublift_core` 职责过重且依赖混杂**: `sublift_core` 目前既包含纯领域模型/算法 (`version`, `hash`, `image`, `models`, `timeline`, `dedupe`, `line_select`)，也包含流式 Pipeline 与特征提取 TU (`signature`, `changepoint`, `pipeline`)。
2. **OpenCV PRIVATE 依赖绑定在 Core**: 当 `SUBLIFT_ENABLE_OPENCV=ON` 时，`sublift_core` PRIVATE 链接了 OpenCV 动态库 (`${OpenCV_LIBS}`) 并包含了 OpenCV 头文件目录 (`${OpenCV_INCLUDE_DIRS}`)，破坏了 `sublift_core` 作为纯粹底层模型/算法库的独立性。
3. **上层消费混淆**: `sublift_application`、`sublift_test_support` 与 `sublift_tests` 目前隐式从 `sublift_core` 继承 `signature` / `changepoint` / `pipeline` 的符号使用，缺乏 Target 级别的显式解耦。

### 1.2 目标 (Target State)
1. **纯洁的 `sublift_core` Target**:
   - `sublift_core` 仅包含纯领域模型与算法 TU (`version.cpp`, `hash.cpp`, `image.cpp`, `models.cpp`, `timeline.cpp`, `dedupe.cpp`, `line_select.cpp`)。
   - 彻底移除对 OpenCV 的依赖 (`${OpenCV_LIBS}`, `${OpenCV_INCLUDE_DIRS}`)，确保 `sublift_core` 不管是否开启 OpenCV 均零 OpenCV 依赖。
2. **独立的 `sublift_pipeline` STATIC Target**:
   - 在 `cpp/src/core/CMakeLists.txt` 中新增 `sublift_pipeline` STATIC 静态库与 `sublift::pipeline` ALIAS。
   - 源文件包含 `signature.cpp`, `changepoint.cpp`, `pipeline.cpp` (当 `SUBLIFT_ENABLE_OPENCV` 为 ON 时)，并显式包含公共头文件 (`signature.hpp`, `changepoint.hpp`, `pipeline.hpp`) 保证在 OpenCV 关闭时 Target 仍能正确生成。
   - PUBLIC 链接 `sublift_core`，PRIVATE 链接 OpenCV (`${OpenCV_LIBS}`, `${OpenCV_INCLUDE_DIRS}`)。
3. **下游 Target 显式链接更新**:
   - `sublift_application` (`cpp/src/worker/CMakeLists.txt`): PUBLIC 链接列表显式增加 `sublift_pipeline`。
   - `sublift_test_support` (`cpp/src/test_support/CMakeLists.txt`): PUBLIC 链接列表显式增加 `sublift_pipeline`。
   - `sublift_tests` (`cpp/tests/CMakeLists.txt`): PRIVATE 链接列表显式增加 `sublift_pipeline`。
4. **范围约束**:
   - 暂不物理搬迁 `cpp/src/core/` 目录到 `cpp/src/pipeline/`（物理目录搬迁归属于 `feat-06905`）。
   - 严禁修改任何打轴、去重、signature、changepoint 的数值逻辑、算法实现或 parity golden 评估数据集。

---

## 2. 有序实施步骤（每步可验证）

### Step 1: 在 `cpp/src/core/CMakeLists.txt` 中拆分 `sublift_pipeline` Target
- **操作**:
  - 重构 `cpp/src/core/CMakeLists.txt`：
    1. 从 `_sublift_core_srcs` 中移除 `signature.cpp`, `changepoint.cpp`, `pipeline.cpp` 的条件追加。
    2. 移除 `sublift_core` 中的 `target_link_libraries(sublift_core PRIVATE ${OpenCV_LIBS})` 与 `target_include_directories(sublift_core PRIVATE ${OpenCV_INCLUDE_DIRS})`。
    3. 声明 `_sublift_pipeline_srcs`，包含 `${PROJECT_SOURCE_DIR}/include/sublift/signature.hpp`, `${PROJECT_SOURCE_DIR}/include/sublift/changepoint.hpp`, `${PROJECT_SOURCE_DIR}/include/sublift/pipeline.hpp`。
    4. 当 `SUBLIFT_ENABLE_OPENCV` 为 ON 时，将 `signature.cpp`, `changepoint.cpp`, `pipeline.cpp` 追加至 `_sublift_pipeline_srcs`。
    5. 声明 `add_library(sublift_pipeline STATIC ${_sublift_pipeline_srcs})` 及 `add_library(sublift::pipeline ALIAS sublift_pipeline)`。
    6. 设置 `target_include_directories(sublift_pipeline PUBLIC ...)` 头文件搜索路径。
    7. 设置 `target_link_libraries(sublift_pipeline PUBLIC sublift_core)`。
    8. 当 `SUBLIFT_ENABLE_OPENCV` 为 ON 时，设置 `target_link_libraries(sublift_pipeline PRIVATE ${OpenCV_LIBS})` 与 `target_include_directories(sublift_pipeline PRIVATE ${OpenCV_INCLUDE_DIRS})`。
- **验证**:
  - 运行 `cmake -B build/cpp cpp` 确保 CMake 配置成功。

### Step 2: 更新下游 Target (`sublift_application`, `sublift_test_support`, `sublift_tests`) 的链接关系
- **操作**:
  - `cpp/src/worker/CMakeLists.txt`:
    - 在 `sublift_application` 的 `target_link_libraries` PUBLIC 列表中加入 `sublift_pipeline`。
  - `cpp/src/test_support/CMakeLists.txt`:
    - 在 `sublift_test_support` 的 `target_link_libraries` PUBLIC 列表中加入 `sublift_pipeline`。
  - `cpp/tests/CMakeLists.txt`:
    - 在 `sublift_tests` 的 `target_link_libraries` PRIVATE 列表中加入 `sublift_pipeline`。
- **验证**:
  - 运行 `cmake -B build/cpp cpp` 成功生成配置。
  - 运行 `cmake --build build/cpp --target sublift_core sublift_pipeline sublift_application sublift_test_support sublift_tests` 顺利完成编译。

### Step 3: Catch2 单元与 Parity 测试验证
- **操作**:
  - 执行 pipeline/signature/changepoint 相关的 Catch2 测试及 parity 测试：
    `ctest --test-dir build/cpp -R 'pipeline|signature|changepoint'`
  - 执行全量 Catch2 单元测试套件：
    `ctest --test-dir build/cpp`
- **验证**:
  - `pipeline`, `signature`, `changepoint` 测试 100% 通过。
  - `ctest --test-dir build/cpp` 全量测试 100% 通过。

### Step 4: Core 纯净度审计与 Baseline 全门例行检查
- **操作**:
  - 审计 `sublift_core` Target 符号与依赖，确认零 OpenCV 符号与链接依赖。
  - 运行项目标准启动门：`./init.sh`
- **验证**:
  - `./init.sh` 顺利执行完毕，ruff, mypy, pytest, Debug C++ build, Catch2 tests 与 parity checks 全成。
  - 零 golden 数据集变更。

---

## 3. 要改 / 新建的文件列表

### 修改文件
- `cpp/src/core/CMakeLists.txt` (剥离 OpenCV 与 signature/changepoint/pipeline，声明 sublift_pipeline Target)
- `cpp/src/worker/CMakeLists.txt` (sublift_application PUBLIC 链接 sublift_pipeline)
- `cpp/src/test_support/CMakeLists.txt` (sublift_test_support PUBLIC 链接 sublift_pipeline)
- `cpp/tests/CMakeLists.txt` (sublift_tests PRIVATE 链接 sublift_pipeline)

### 新建文件
- `docs/cpp/plans/feat-06903-plan.md` (本计划文档)

---

## 4. 测试计划与验收命令

1. **Pipeline / Signature / Changepoint 针对性测试**:
   `ctest --test-dir build/cpp -R 'pipeline|signature|changepoint'`
2. **C++ 全量 Catch2 单元测试**:
   `ctest --test-dir build/cpp`
3. **日常全门验证 (Baseline Gate)**:
   `./init.sh`

---

## 5. 风险与回滚

- **风险 1**: `sublift_pipeline` 在 OpenCV 关闭 (`SUBLIFT_ENABLE_OPENCV=OFF`) 时因无源文件导致 CMake 构建生成失败。
  - **规避**: 在 `_sublift_pipeline_srcs` 中默认挂载公共头文件 (`signature.hpp`, `changepoint.hpp`, `pipeline.hpp`)，确保 CMake target 始终有源定义。
- **风险 2**: `sublift_test_support` 丢失 `sublift_pipeline` 链接导致 `signature_golden.cpp` / `pipeline_golden.cpp` 报未定义符号错误。
  - **规避**: 在 `sublift_test_support` 的 PUBLIC `target_link_libraries` 中显式引入 `sublift_pipeline`。
- **回滚方案**: 若出现无法解决的构建冲突，可执行 `git checkout -- cpp/` 恢复代码状态。

---

## 6. 非目标与约束再确认 (Non-Goals)

1. **不进行** `cpp/src/core/` 到 `cpp/src/pipeline/` 的物理目录搬迁（归属于 feat-06905）。
2. **不修改** signature、changepoint、pipeline、timeline、dedupe 的打轴与去重数值逻辑。
3. **不改动** 算法、golden 评估数据集与 DTO/接口数据结构。

---

## 7. Done 定义

- [ ] 在 `cpp/src/core/CMakeLists.txt` 中声明 `sublift_pipeline` STATIC target。
- [ ] `sublift_core` 仅包含纯领域模型与算法 (`version`, `hash`, `image`, `models`, `timeline`, `dedupe`, `line_select`)，且 100% 移除了对 OpenCV 的依赖。
- [ ] `sublift_pipeline` 包含 `signature.cpp`, `changepoint.cpp`, `pipeline.cpp` (当 `SUBLIFT_ENABLE_OPENCV` 为 ON 时)，PUBLIC 链接 `sublift_core`，PRIVATE 链接 OpenCV。
- [ ] `sublift_application` (`cpp/src/worker/CMakeLists.txt`) PUBLIC 链接增加 `sublift_pipeline`。
- [ ] `sublift_test_support` (`cpp/src/test_support/CMakeLists.txt`) PUBLIC 链接增加 `sublift_pipeline`。
- [ ] `sublift_tests` (`cpp/tests/CMakeLists.txt`) PRIVATE 链接增加 `sublift_pipeline`。
- [ ] `ctest --test-dir build/cpp -R 'pipeline|signature|changepoint'` 执行且全绿。
- [ ] `ctest --test-dir build/cpp` 执行且全绿。
- [ ] `./init.sh` 全门验证通过，零错误且无 golden 变更。
