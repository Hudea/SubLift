# feat-06906 可执行计划：Paddle 产品公共面收缩

- **Feature ID**: feat-06906
- **Feature Name**: Paddle 产品公共面收缩
- **Phase**: Phase 6.9 (Native 产品架构整理)
- **Worktree**: `/Volumes/lab/pp/SubLift_CPP`
- **关联设计**: `docs/cpp/phase6.9-native-product-architecture.md` / `docs/cpp/phase6.9-implementation-plan.md`

---

## 1. 现状与目标 Diff

### 1.1 现状 (Current State)
1. **Include 路径与分层混乱**:
   - `cpp/include/sublift/paddle_geometry.hpp` 存放在根 include 目录 `cpp/include/sublift/` 中，未按照 ports 分层原则归位至 `cpp/include/sublift/ports/`。
   - `cpp/include/sublift/ports/paddle.hpp` 仍硬编码引用 `#include "sublift/paddle_geometry.hpp"`。
   - `cpp/include/sublift/paddle.hpp` 已为指向 `<sublift/ports/paddle.hpp>` 的转发头，但 `paddle_geometry.hpp` 尚未建立 `cpp/include/sublift/ports/paddle_geometry.hpp` 物理存放路径及对应的根路径转发头。
2. **公共面暴露审计**:
   - `cpp/include/sublift/ports/paddle.hpp` 暴露了 `PaddleOcrEngine` (继承 `IOcrEngine`)、`is_paddle_available()`、`PaddleOcrOptions`、`PaddleRuntimeStats` 以及诊断用的 `PaddleStageTrace` 及其附属 Trace 结构 (`PaddleTensorTrace`, `PaddleQuadTrace`, `PaddleCropStageTrace`, `PaddleClsResultTrace`, `PaddleDecodedTrace`)。公共接口定义清晰且纯粹，未向外部暴露任何 PP-OCR 内部中间数据结构或推理节点细节。
3. **内部实现隔离**:
   - 内部 PP-OCR 阶段头文件 (`ppocr_det_preprocess.hpp`, `ppocr_db_postprocess.hpp`, `ppocr_crop_cls.hpp`, `ppocr_ctc.hpp`, `paddle_models.hpp`) 均保存在 `cpp/src/adapters/paddle/` 内部目录中，未泄漏至公共 `cpp/include/sublift/`，但需在计划中显式设定规范性收紧校验。

### 1.2 目标 (Target State)
1. **Header 位置规范化与转发头支持**:
   - 将 `cpp/include/sublift/paddle_geometry.hpp` 物理移动至 `cpp/include/sublift/ports/paddle_geometry.hpp`。
   - 在 `cpp/include/sublift/paddle_geometry.hpp` 原位置建立 100% 向后兼容的转发头（内容为 `#pragma once\n#include <sublift/ports/paddle_geometry.hpp>`）。
   - 将 `cpp/include/sublift/ports/paddle.hpp` 中的包含路径更新为 `#include "sublift/ports/paddle_geometry.hpp"`。
2. **公共面精准定义**:
   - 确认 `cpp/include/sublift/ports/paddle.hpp` 为 Paddle OCR 模块在 `sublift` 公共 include 面上的唯一对外头文件，严格仅暴露：
     - `is_paddle_available()`
     - `PaddleOcrOptions`
     - `PaddleRuntimeStats`
     - `PaddleOcrEngine` (继承 `IOcrEngine`)
     - `PaddleStageTrace` (诊断/调试 Trace 结构及相关 Trace 子结构)
3. **内部实现硬封闭确认**:
   - 封锁并锁定所有 PP-OCR 阶段及模型辅助头文件 (`ppocr_det_preprocess.hpp`, `ppocr_db_postprocess.hpp`, `ppocr_crop_cls.hpp`, `ppocr_ctc.hpp`, `paddle_models.hpp`) 于 `cpp/src/adapters/paddle/` 内部，禁止进入公共 include 目录。
4. **零算法 & 零 Golden 变动**:
   - 不修改 PP-OCRv6 Det/Rec/Cls 算法、Postprocess 阈值或模型逻辑；不修改 RapidOCR 缓存路径；保持测试与 Golden 数据集零 Diff。

---

## 2. 有序实施步骤（每步可验证）

### Step 1: 移动 `paddle_geometry.hpp` 并配置向后兼容转发头
- **操作**:
  - 执行 `git mv cpp/include/sublift/paddle_geometry.hpp cpp/include/sublift/ports/paddle_geometry.hpp`。
  - 创建转发头 `cpp/include/sublift/paddle_geometry.hpp`：
    ```cpp
    #pragma once
    #include <sublift/ports/paddle_geometry.hpp>
    ```
  - 修改 `cpp/include/sublift/ports/paddle.hpp` 中的引用为 `#include "sublift/ports/paddle_geometry.hpp"`。
- **验证**:
  - 运行 `git status` 确认 `paddle_geometry.hpp` 呈现为 clean rename 移动。

### Step 2: 修复 C++ 内部 TU 与测试的 Includes 引用
- **操作**:
  - 机械替换以下源文件中的 `#include "sublift/paddle_geometry.hpp"` 为 `#include "sublift/ports/paddle_geometry.hpp"`：
    - `cpp/src/adapters/paddle/paddle_geometry.cpp`
    - `cpp/src/adapters/paddle/ppocr_ctc.hpp`
    - `cpp/src/adapters/paddle/ppocr_db_postprocess.hpp`
    - `cpp/tests/paddle_geometry_test.cpp`
    - `cpp/tests/parity/paddle_parity_test.cpp`
- **验证**:
  - 审计所有 `.hpp`/`.cpp`，确认除兼容性测试外，项目内部 TU 统一使用分层后的 `sublift/ports/paddle_geometry.hpp` 包含路径。

### Step 3: 公共面 API 与内部头封禁性审计
- **操作**:
  - 审查 `cpp/include/sublift/ports/paddle.hpp`，确认公共 API 仅由 `PaddleOcrEngine`、`is_paddle_available()`、`PaddleOcrOptions`、`PaddleRuntimeStats` 以及 `PaddleStageTrace` 及其关联 trace 结构组成。
  - 检查 `cpp/include/sublift/` 及其子目录，确认内部阶段头文件 (`ppocr_det_preprocess.hpp`, `ppocr_db_postprocess.hpp`, `ppocr_crop_cls.hpp`, `ppocr_ctc.hpp`, `paddle_models.hpp`) 均在 `cpp/src/adapters/paddle/` 内部，未泄露至 `cpp/include/sublift/`。
- **验证**:
  - grep 搜索 `ppocr_` 确认外部代码无法直接引用内部私有阶段头文件。

### Step 4: CTest Paddle 专项测试验证
- **操作**:
  - 运行 `ctest --test-dir build/cpp -R 'paddle|Paddle'`
- **验证**:
  - 100% 跑通 Paddle 几何转换、CTC 解码、Crop/Cls、DB 后处理、Stub 及 Parity/Stages 测试。

### Step 5: 全日常门校验 (`./init.sh`)
- **操作**:
  - 运行 `./init.sh`
- **验证**:
  - `ruff` / `mypy` / `ctest` / `pytest` 及 cutover parity 检查全绿。

---

## 3. 要改 / 新建 / 移动的文件列表

### 移动文件 (`git mv`)
- `cpp/include/sublift/paddle_geometry.hpp` -> `cpp/include/sublift/ports/paddle_geometry.hpp`

### 新建文件
- `cpp/include/sublift/paddle_geometry.hpp` (向后兼容转发头)
- `docs/cpp/plans/feat-06906-plan.md` (本计划文档)

### 修改文件
- `cpp/include/sublift/ports/paddle.hpp`
- `cpp/src/adapters/paddle/paddle_geometry.cpp`
- `cpp/src/adapters/paddle/ppocr_ctc.hpp`
- `cpp/src/adapters/paddle/ppocr_db_postprocess.hpp`
- `cpp/tests/paddle_geometry_test.cpp`
- `cpp/tests/parity/paddle_parity_test.cpp`

---

## 4. 测试计划与验收命令

1. **Catch2 Paddle 专项测试**:
   `ctest --test-dir build/cpp -R 'paddle|Paddle'`
2. **日常全门验证 (Baseline Gate)**:
   `./init.sh`

---

## 5. 风险与回滚

- **风险**: 外部或部分旧测试代码在未更新路径时编译找不到 `paddle_geometry.hpp`。
  - **规避**: 在 `cpp/include/sublift/paddle_geometry.hpp` 保留转发头 `#include <sublift/ports/paddle_geometry.hpp>`。
- **回滚方案**: `git reset --hard HEAD`

---

## 6. 非目标与约束再确认 (Non-Goals)

1. **不修改** PP-OCRv6 Det/Rec/Cls 算法、Postprocess 阈值或模型逻辑。
2. **不修改** Python / C++ RapidOCR 缓存路径机制。
3. **严禁改动** 算法、golden 评估数据集与字符解码策略。

---

## 7. Done 定义

- [ ] `paddle_geometry.hpp` 完成移动至 `cpp/include/sublift/ports/paddle_geometry.hpp`。
- [ ] `cpp/include/sublift/paddle_geometry.hpp` 建立无缝转发头。
- [ ] `cpp/include/sublift/ports/paddle.hpp` 确认严格仅暴露 `PaddleOcrEngine` (继承 `IOcrEngine`)、`is_paddle_available()`、`PaddleOcrOptions`、`PaddleRuntimeStats` 以及诊断用 `PaddleStageTrace` 及其 Trace 附属结构。
- [ ] 确认 PP-OCR 内部阶段头文件 (`ppocr_det_preprocess.hpp`, `ppocr_db_postprocess.hpp`, `ppocr_crop_cls.hpp`, `ppocr_ctc.hpp`, `paddle_models.hpp`) 紧锁在 `cpp/src/adapters/paddle/` 内部。
- [ ] `ctest --test-dir build/cpp -R 'paddle|Paddle'` 100% 绿。
- [ ] `./init.sh` 验证全绿。
