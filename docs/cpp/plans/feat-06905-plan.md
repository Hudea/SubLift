# feat-06905 可执行计划：机械目录与 include 布局

- **Feature ID**: feat-06905
- **Feature Name**: 机械目录与 include 布局
- **Phase**: Phase 6.9 (Native 产品架构整理)
- **Worktree**: `/Volumes/lab/pp/SubLift_CPP`
- **关联设计**: `docs/cpp/phase6.9-native-product-architecture.md` / `docs/cpp/phase6.9-implementation-plan.md`

---

## 1. 现状与目标 Diff

### 1.1 现状 (Current State)
1. **物理目录比较平铺**:
   - Adapter 引擎实现（FFmpeg、PaddleOCR、Vision macOS）平铺存储于 `cpp/src/ffmpeg/`、`cpp/src/paddle/` 和 `cpp/src/vision_macos/`。
   - Pipeline 相关逻辑（`signature.*`, `changepoint.*`, `pipeline.*`）混杂存放在 `cpp/src/core/` 中。
   - Worker 协议层（`framing.*`, `protocol.*`）与 Bridge 应用编排层（`bridge.*`, `bridge_no_opencv.cpp`）混合存放在 `cpp/src/worker/` 下。
   - Paddle 诊断工具 `paddle_trace_main.cpp` 放在 `cpp/src/paddle/` Adapter 源码目录中。
2. **Public Include 未分类分层**:
   - `cpp/include/sublift/` 下所有头文件平铺在单一路径下，缺乏 `core`, `ports`, `pipeline`, `protocol` 的清晰分层。
3. **CMake 配置依赖旧平铺路径**:
   - 根 `cpp/CMakeLists.txt` 及各子目录 `CMakeLists.txt` 均硬编码旧有的物理相对路径。

### 1.2 目标 (Target State)
1. **机械物理目录重构 (使用 `git mv`)**:
   - Adapters 适配器合拢：
     - `cpp/src/ffmpeg/` -> `cpp/src/adapters/ffmpeg/`
     - `cpp/src/paddle/` -> `cpp/src/adapters/paddle/`
     - `cpp/src/vision_macos/` -> `cpp/src/adapters/vision_macos/`
   - Diagnostic 诊断工具剥离：
     - `cpp/src/paddle/paddle_trace_main.cpp` -> `cpp/diagnostics/paddle_trace/paddle_trace_main.cpp`
   - Protocol 协议沉淀：
     - `cpp/src/worker/framing.*`, `protocol.*` -> `cpp/src/protocol/`
   - Application 应用编排移位：
     - `cpp/src/worker/bridge.*`, `bridge_no_opencv.cpp` -> `cpp/src/application/`
   - Pipeline 独立拆分：
     - `cpp/src/core/signature.*`, `changepoint.*`, `pipeline.*` -> `cpp/src/pipeline/`
2. **Include 分层与兼容转发头**:
   - 建立 `cpp/include/sublift/{core,ports,pipeline,protocol}/` 分层目录结构，并将主头文件移动到对应层次中。
   - 保留原根 `cpp/include/sublift/*.hpp` 头文件作为转发头（如 `#include <sublift/core/image.hpp>`），保持全项目及外部依赖 100% 绝对兼容。
3. **CMakeLists 路径更新**:
   - 更新根 `cpp/CMakeLists.txt` 及各子目录 `CMakeLists.txt` 对应的物理路径与 `target_include_directories`，确保编译构建无缝过度。
4. **零算法与零 Golden Diff**:
   - 不修改任何函数实现、去重/打轴/OCR 算法逻辑，保证 Golden 测试与 Baseline 评估零差异。

---

## 2. 有序实施步骤（每步可验证）

### Step 1: 机械目录调整 (`git mv`)
- **操作**:
  - 创建目标目录：
    `mkdir -p cpp/src/adapters cpp/src/pipeline cpp/src/protocol cpp/src/application cpp/diagnostics/paddle_trace cpp/include/sublift/{core,ports,pipeline,protocol}`
  - 使用 `git mv` 进行机械移动：
    - `git mv cpp/src/ffmpeg cpp/src/adapters/ffmpeg`
    - `git mv cpp/src/vision_macos cpp/src/adapters/vision_macos`
    - `git mv cpp/src/paddle/paddle_trace_main.cpp cpp/diagnostics/paddle_trace/`
    - `git mv cpp/src/paddle cpp/src/adapters/paddle`
    - `git mv cpp/src/worker/framing.cpp cpp/src/worker/framing.hpp cpp/src/worker/protocol.cpp cpp/src/worker/protocol.hpp cpp/src/protocol/`
    - `git mv cpp/src/worker/bridge.cpp cpp/src/worker/bridge.hpp cpp/src/worker/bridge_no_opencv.cpp cpp/src/application/`
    - `git mv cpp/src/core/signature.cpp cpp/src/core/signature.hpp cpp/src/core/changepoint.cpp cpp/src/core/changepoint.hpp cpp/src/core/pipeline.cpp cpp/src/core/pipeline.hpp cpp/src/pipeline/` (按头文件/源文件实际归属移动)
- **验证**:
  - 运行 `git status` 确认所有变动均呈现为 clean rename 记录。

### Step 2: `include/sublift/` 分层建立与转发兼容头配置
- **操作**:
  - 将公共头文件移动至分层目录：
    - `core/`: `models.hpp`, `config.hpp`, `image.hpp`, `hash.hpp`, `timeline.hpp`, `dedupe.hpp`, `line_select.hpp`, `version.hpp`
    - `ports/`: `ocr.hpp`, `detector.hpp`, `extractor.hpp`, `fixed_detector.hpp`, `bottom_crop_detector.hpp`, `roi_passthrough_detector.hpp`, `mock_ocr.hpp`, `ffmpeg.hpp`, `vision.hpp`, `paddle.hpp`
    - `pipeline/`: `pipeline.hpp`, `signature.hpp`, `changepoint.hpp`
    - `protocol/`: `protocol.hpp`, `framing.hpp`
  - 在 `cpp/include/sublift/` 根路径下重新创建同名转发兼容头（例如在 `cpp/include/sublift/image.hpp` 中写入 `#pragma once\n#include <sublift/core/image.hpp>`）。
- **验证**:
  - 检查 `cpp/include/sublift/` 下所有头文件均成功建立转发。

### Step 3: 更新与新建 CMakeLists.txt 物理路径配置
- **操作**:
  - 更新 `cpp/CMakeLists.txt` 中的 `add_subdirectory` 路径。
  - 为 `src/pipeline`, `src/protocol`, `src/application`, `diagnostics/paddle_trace` 创建/调整对应的 `CMakeLists.txt`。
  - 更新 `src/core/CMakeLists.txt`、`src/worker/CMakeLists.txt` 和各 adapter 的 `CMakeLists.txt`。
  - 确保各 target 的 `target_include_directories` 包含 `${CMAKE_CURRENT_SOURCE_DIR}/include`。
- **验证**:
  - 运行 `cmake -B build/cpp cpp` 检查配置无报错。
  - 运行 `cmake --build build/cpp` 完成编译。

### Step 4: Include 包含路径机械修复
- **操作**:
  - 机械替换移位后的 C++ 源文件中的相对 `#include` 路径，确保引用符合新的分层布局或保持兼容转发路径。
- **验证**:
  - 运行 `cmake --build build/cpp` 顺利编译所有组件。

### Step 5: CTest 全单元测试验证与 Baseline 全门例行检查
- **操作**:
  - 运行 `ctest --test-dir build/cpp`
  - 运行 `./init.sh`
  - 运行 `git diff -- tests/ fixtures/ tests/golden/`
- **验证**:
  - CTest 单元测试 100% 通过。
  - `./init.sh` 全门跑通。
  - Golden/Fixture 评价数据集无任何 diff。

---

## 3. 要改 / 新建 / 移动的文件列表

### 移动文件 (`git mv`)
- `cpp/src/ffmpeg/` -> `cpp/src/adapters/ffmpeg/`
- `cpp/src/paddle/` -> `cpp/src/adapters/paddle/` (除 `paddle_trace_main.cpp`)
- `cpp/src/paddle/paddle_trace_main.cpp` -> `cpp/diagnostics/paddle_trace/paddle_trace_main.cpp`
- `cpp/src/vision_macos/` -> `cpp/src/adapters/vision_macos/`
- `cpp/src/worker/framing.*`, `protocol.*` -> `cpp/src/protocol/`
- `cpp/src/worker/bridge.*`, `bridge_no_opencv.cpp` -> `cpp/src/application/`
- `cpp/src/core/signature.*`, `changepoint.*`, `pipeline.*` -> `cpp/src/pipeline/`
- `cpp/include/sublift/*.hpp` -> `cpp/include/sublift/{core,ports,pipeline,protocol}/*.hpp`

### 新建转发兼容头
- `cpp/include/sublift/config.hpp` (`#include <sublift/core/config.hpp>`)
- `cpp/include/sublift/dedupe.hpp` (`#include <sublift/core/dedupe.hpp>`)
- `cpp/include/sublift/hash.hpp` (`#include <sublift/core/hash.hpp>`)
- `cpp/include/sublift/image.hpp` (`#include <sublift/core/image.hpp>`)
- `cpp/include/sublift/line_select.hpp` (`#include <sublift/core/line_select.hpp>`)
- `cpp/include/sublift/models.hpp` (`#include <sublift/core/models.hpp>`)
- `cpp/include/sublift/timeline.hpp` (`#include <sublift/core/timeline.hpp>`)
- `cpp/include/sublift/version.hpp` (`#include <sublift/core/version.hpp>`)
- `cpp/include/sublift/ocr.hpp` (`#include <sublift/ports/ocr.hpp>`)
- `cpp/include/sublift/detector.hpp` (`#include <sublift/ports/detector.hpp>`)
- `cpp/include/sublift/extractor.hpp` (`#include <sublift/ports/extractor.hpp>`)
- `cpp/include/sublift/fixed_detector.hpp` (`#include <sublift/ports/fixed_detector.hpp>`)
- `cpp/include/sublift/bottom_crop_detector.hpp` (`#include <sublift/ports/bottom_crop_detector.hpp>`)
- `cpp/include/sublift/roi_passthrough_detector.hpp` (`#include <sublift/ports/roi_passthrough_detector.hpp>`)
- `cpp/include/sublift/mock_ocr.hpp` (`#include <sublift/ports/mock_ocr.hpp>`)
- `cpp/include/sublift/ffmpeg.hpp` (`#include <sublift/ports/ffmpeg.hpp>`)
- `cpp/include/sublift/vision.hpp` (`#include <sublift/ports/vision.hpp>`)
- `cpp/include/sublift/paddle.hpp` (`#include <sublift/ports/paddle.hpp>`)
- `cpp/include/sublift/pipeline.hpp` (`#include <sublift/pipeline/pipeline.hpp>`)
- `cpp/include/sublift/signature.hpp` (`#include <sublift/pipeline/signature.hpp>`)
- `cpp/include/sublift/changepoint.hpp` (`#include <sublift/pipeline/changepoint.hpp>`)
- `cpp/include/sublift/protocol.hpp` (`#include <sublift/protocol/protocol.hpp>`)
- `cpp/include/sublift/framing.hpp` (`#include <sublift/protocol/framing.hpp>`)

### 修改与新建 CMakeLists.txt
- `cpp/CMakeLists.txt`
- `cpp/src/core/CMakeLists.txt`
- `cpp/src/pipeline/CMakeLists.txt` (新建)
- `cpp/src/protocol/CMakeLists.txt` (新建)
- `cpp/src/application/CMakeLists.txt` (新建)
- `cpp/diagnostics/paddle_trace/CMakeLists.txt` (新建)
- `cpp/src/adapters/ffmpeg/CMakeLists.txt`
- `cpp/src/adapters/paddle/CMakeLists.txt`
- `cpp/src/adapters/vision_macos/CMakeLists.txt`
- `cpp/src/worker/CMakeLists.txt`
- `cpp/src/cli/CMakeLists.txt`
- `cpp/tests/CMakeLists.txt`

### 新建计划文档
- `docs/cpp/plans/feat-06905-plan.md` (本计划文件)

---

## 4. 测试计划与验收命令

1. **Catch2 单元与集成测试**:
   `ctest --test-dir build/cpp`
2. **日常全门验证 (Baseline Gate)**:
   `./init.sh`
3. **Golden 与 Fixture 零差异校验**:
   `git diff -- tests/ fixtures/ tests/golden/`

---

## 5. 风险与回滚

- **风险 1**: 某些单元测试或工具缺少某个转发头导致编译失败。
  - **规避**: 对 `cpp/include/sublift/*.hpp` 所有原文件均逐一产生对应的转发头，保留根路径包含兼容能力。
- **风险 2**: `CMakeLists.txt` 在拆分新 target 时 include path 未添加全局 include 目录。
  - **规避**: 统一把 `${CMAKE_CURRENT_SOURCE_DIR}/include` 加入对应 Target 的 `PUBLIC` 或 `INTERFACE` 目录中。
- **回滚方案**: 执行 `git reset --hard HEAD` 撤销物理目录更改。

---

## 6. 非目标与约束再确认 (Non-Goals)

1. **不包含** Paddle 公共 API 的收缩（属于 `feat-06906`）。
2. **不修改** 任何 C++ 函数实现或算法逻辑（OCR/去重/时间轴/signature/changepoint/framing 逻辑必须保真）。
3. **不修改** 字符串/数值语义或 SRT 输出格式。

---

## 7. Done 定义

- [ ] 完成 `git mv` 物理目录整理（`adapters/{ffmpeg,paddle,vision_macos}`, `protocol`, `application`, `pipeline`, `diagnostics/paddle_trace`）。
- [ ] 建立 `cpp/include/sublift/{core,ports,pipeline,protocol}/` 分层，头文件正确移动归位。
- [ ] `cpp/include/sublift/*.hpp` 重新提供无缝转发头。
- [ ] `CMakeLists.txt` 物理路径与 Target 配置全部更新。
- [ ] `ctest --test-dir build/cpp` 100% 绿。
- [ ] `./init.sh` 日常启动门验证全绿。
- [ ] `git diff` 确认 Golden 及 Fixtures 零差异。
