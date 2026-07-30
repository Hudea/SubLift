# feat-06913 可执行计划：Fallback 退役准备与 Phase 6 终态清理

- **Feature ID**: feat-06913
- **Feature Name**: Fallback 退役准备与 Phase 6 终态清理
- **Phase**: Phase 6.9 (Native 产品架构整理 / 去 Python 分发)
- **Worktree**: `/Volumes/lab/pp/SubLift_CPP`
- **关联设计**: `docs/cpp/phase6.9-native-product-architecture.md` / `docs/cpp/engine-matrix-and-cutover.md` / `docs/cpp/phase6.9-implementation-plan.md` / `docs/ARCHITECTURE.md`

---

## 1. 现状与目标 Diff

### 1.1 现状 (Current State)
1. **Phase 6.9 Native 产品架构组装进度**:
   - `feat-06901` ~ `feat-06912` 均已 100% 交付落地（包括 C++ Target 图、IPC protocol/application/pipeline 拆分、ResourceLocator、Native CLI、macOS App Bundle 布局、Python-free 隔离运行校验门等）。
   - `feat-06913` 为 Phase 6.9 与 Phase 6 的最后一个收口任务，负责梳理终态文档、厘清引擎矩阵与回滚边界，并更新整体状态至 `done`。
2. **引擎矩阵与 Python 角色定位**:
   - Paddle Native 在 macOS 上已达到生产级 stable 引擎标准。
   - 现有的 Phase 6.9 文档、`engine-matrix-and-cutover.md` 和 `ARCHITECTURE.md` 需要更新统一口径：明确 Paddle Native 为 macOS 上的生产 stable 引擎；而 Python rapidocr 逻辑保留在代码库中作为 Oracle 比对、benchmark 评估以及显式 `--runtime python` 调试/回滚手段（不物理删除 `src/sublift/ocr/paddle.py`）。
3. **状态跟踪与导航节点 (`feature-list.json`, `docs/phases/phase6.json`, `progress.md`)**:
   - `feature-list.json` 中 `phase6.post-cutover` (Phase 6.9) 状态仍为 `"in-progress"`。
   - `docs/phases/phase6.json` 中 `feat-06913` 状态仍为 `"not-started"`。
   - `progress.md` 仍记载 `feat-06913` 为下一实现。

### 1.2 目标 (Target State)
1. **梳理并更新 Phase 6 / 6.9 终态架构文档**:
   - `docs/cpp/engine-matrix-and-cutover.md`：
     - 标明 Phase 6.9 Native 产品架构已全部完成组装 (feat-06901 ~ feat-06913 100% 交付)。
     - 明确 Paddle Native 为 macOS 上的生产 stable 引擎，Python rapidocr 保留为 Oracle 比对与显式 `--runtime python` 调试/回滚手段。
   - `docs/cpp/phase6.9-native-product-architecture.md`：
     - 在状态声明和相关章节标明 Phase 6.9 Native 产品架构已 100% 交付，所有 13 个 feature (feat-06901 ~ feat-06913) 全部完成。
   - `docs/ARCHITECTURE.md`：
     - 更新 Phase 6 终态描述，确认 Native C++ 架构与分发收口完成，明确 Paddle Native 生产默认与 Python 侧作为 Oracle / Benchmark / 显式回滚的长期定位。
2. **更新项目级功能总览 `feature-list.json`**:
   - 将 `phase6.post-cutover` (Phase 6.9) 的整体状态更新为 `"done"`。
3. **更新 Phase 6 详细跟踪 `docs/phases/phase6.json`**:
   - 将 `feat-06913` 状态更新为 `"done"`，并填写完整的完成证据 (evidence)。
4. **更新导航日志 `progress.md`**:
   - 更新当前状态，标记 Phase 6.9 100% 完成，将 `feat-06913` 放入近期完成列表。
5. **通过全日常门与 Parity 门验证**:
   - 执行 `python3 scripts/parity/check_cutover_gate.py --check --skip-runtime --skip-gt`。
   - 执行 `./init.sh`。

### 1.3 绝对约束与非目标 (In Scope / Out of Scope / Prohibition)
- **In Scope**:
  1. 梳理并更新 Phase 6 终态文档 (`docs/cpp/engine-matrix-and-cutover.md`, `docs/cpp/phase6.9-native-product-architecture.md`, `docs/ARCHITECTURE.md`)。
  2. 更新 `feature-list.json`：将 Phase 6.9 (`phase6.post-cutover`) 状态更新为 `"done"`。
  3. 更新 `docs/phases/phase6.json` 和 `progress.md`：将 `feat-06913` 标为 `"done"`。
- **Out of Scope**:
  1. 物理删除 Python `src/sublift/ocr/paddle.py` 逻辑（保留 Python Oracle 比对与 benchmark）。
- **绝对禁止**:
  - 改动算法、golden 评估数据集与 C++ Target 架构。

---

## 2. 有序实施步骤（每步可验证）

### Step 1: 更新终态架构与引擎矩阵文档
- **操作**:
  - 更新 `docs/cpp/engine-matrix-and-cutover.md`：标注 Phase 6.9 架构全量交付，明确 Paddle Native 为 macOS 生产 stable 引擎，Python 路径保留为 Oracle 比对与 `--runtime python` 显式调试/回滚手段。
  - 更新 `docs/cpp/phase6.9-native-product-architecture.md`：在状态与导言部分标注 feat-06901 ~ feat-06913 100% 交付与 Phase 6 终态达到。
  - 更新 `docs/ARCHITECTURE.md`：更新 Phase 1.1 与 6.9 状态描述，明确 Native C++ 终态与 Python Oracle 职责界限。
- **验证**:
  - `git diff docs/cpp/engine-matrix-and-cutover.md docs/cpp/phase6.9-native-product-architecture.md docs/ARCHITECTURE.md` 检查文字精准无歧义。

### Step 2: 更新 `feature-list.json`、`docs/phases/phase6.json` 与 `progress.md`
- **操作**:
  - 更新 `feature-list.json` 中 `"id": "phase6.post-cutover"` 的 `"status"` 为 `"done"`。
  - 更新 `docs/phases/phase6.json` 中 `"id": "feat-06913"` 的 `"status"` 为 `"done"`，并在 `"evidence"` 中填写入库与 gate 验证记录。
  - 更新 `progress.md`：将 Phase 6.9 设为完成，清理临时工程信息，保留精简导航。
- **验证**:
  - `python3 -c "import json; json.load(open('feature-list.json')); json.load(open('docs/phases/phase6.json'))"` 验证 JSON 格式无语法错。

### Step 3: Cutover Gate 与 Parity 校验
- **操作**:
  - 运行 `python3 scripts/parity/check_cutover_gate.py --check --skip-runtime --skip-gt`。
- **验证**:
  - 确认 Cutover gate 返回 0 且无错误。

### Step 4: 日常全门验证 (`./init.sh`)
- **操作**:
  - 执行 `./init.sh`。
- **验证**:
  - `ruff` / `mypy` / `ctest` / `pytest` 及 cutover parity 检查全绿。

---

## 3. 要改 / 新建 / 移动的文件列表

### 新建文件
- `docs/cpp/plans/feat-06913-plan.md` (本计划文档)

### 修改文件
- `docs/cpp/engine-matrix-and-cutover.md` (更新终态矩阵与 Phase 6.9 100% 交付描述)
- `docs/cpp/phase6.9-native-product-architecture.md` (更新状态为 100% 交付完成)
- `docs/ARCHITECTURE.md` (更新 Native C++ 终态与 Python Oracle 定位)
- `feature-list.json` (将 Phase 6.9 状态更新为 done)
- `docs/phases/phase6.json` (将 feat-06913 状态更新为 done 并补充 evidence)
- `progress.md` (更新 Phase 6.9 导航状态)

---

## 4. 测试计划与验收命令

1. **Cutover Parity 门控校验**:
   ```bash
   python3 scripts/parity/check_cutover_gate.py --check --skip-runtime --skip-gt
   ```
2. **日常全门验证 (Baseline Gate)**:
   ```bash
   ./init.sh
   ```

---

## 5. 风险与回滚

- **风险**:
  1. JSON 格式文件 (`feature-list.json`, `docs/phases/phase6.json`) 编辑时由于逗号或括号遗漏导致非法 JSON 语法。
     - **规避**: 使用 Python `json.load` 校验工具在线验证 JSON 文件合法性。
  2. 文档修改时误触或模糊 Paddle Native 生产定位与 Python Oracle 回滚的界限。
     - **规避**: 统一严格按照范围卡描述，明确 Paddle Native 生产默认，Python rapidocr 作为 Oracle 比对与显式 `--runtime python` 手段留存。
- **回滚方案**: `git reset --hard HEAD`

---

## 6. 非目标与约束再确认 (Non-Goals)

1. **绝对禁止** 物理删除 Python `src/sublift/ocr/paddle.py` 逻辑（保留 Python Oracle 比对与 benchmark 评估）。
2. **绝对禁止** 改动算法、golden 评估数据集与 C++ Target 架构。

---

## 7. Done 定义

- [ ] 梳理并更新 `docs/cpp/engine-matrix-and-cutover.md`，明确 Phase 6.9 (feat-06901 ~ feat-06913) 100% 交付与 Paddle Native 生产 stable 定位。
- [ ] 梳理并更新 `docs/cpp/phase6.9-native-product-architecture.md` 状态声明为全量完成。
- [ ] 梳理并更新 `docs/ARCHITECTURE.md` 中的 Phase 6 终态与分层说明。
- [ ] 更新 `feature-list.json` 将 `Phase 6.9` (`phase6.post-cutover`) 状态设为 `"done"`。
- [ ] 更新 `docs/phases/phase6.json` 将 `feat-06913` 状态设为 `"done"` 并记录验证 evidence。
- [ ] 更新 `progress.md` 导航节点。
- [ ] 物理保留 `src/sublift/ocr/paddle.py` Python 逻辑。
- [ ] 验收命令 `python3 scripts/parity/check_cutover_gate.py --check --skip-runtime --skip-gt` 执行 100% PASS。
- [ ] 验收命令 `./init.sh` 执行 100% PASS。
