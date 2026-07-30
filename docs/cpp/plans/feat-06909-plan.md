# feat-06909 可执行计划：Native CLI 产品 parity

- **Feature ID**: feat-06909
- **Feature Name**: Native CLI 产品 parity
- **Phase**: Phase 6.9 (Native 产品架构整理)
- **Worktree**: `/Volumes/lab/pp/SubLift_CPP`
- **关联设计**: `docs/cpp/phase6.9-native-product-architecture.md` / `docs/cpp/phase6.9-implementation-plan.md`

---

## 1. 现状与目标 Diff

### 1.1 现状 (Current State)
1. **Native CLI 引擎选项受限**:
   - `sublift_cli` (`cpp/src/cli/main.cpp`) 参数解析仅支持 `--engine mock|vision`；当传入 `--engine paddle` 时报错 `"Error: native CLI supports --engine mock|vision only. Use uv run sublift extract --engine paddle for PaddleOCR."` 并退出。
2. **CLI Usage 帮助文案过时脱节**:
   - `print_usage` 中包含过时短语 `"Paddle remains on the Python path (uv run sublift --engine paddle)"`，与 Native C++ Core 已完成 Paddle 接入的现状不符。
3. **Worker 身份信息输出未与 Handshake 握手透传同一化**:
   - CLI 在启动提取前仅简单打印手写日志 `提取字幕 (native C++ Worker)：...`，未从 UDS IPC Handshake 的 `ByeMsg` 握手报文中提取 Worker 实际暴露的 `runtime`、`engines` 及 `capabilities` 元数据，导致 Worker 实际身份在 CLI 侧透明度不足。
4. **定位文档缺乏对 Native CLI 与 Python 回归工具的清晰表达**:
   - `README.md` 与 `docs/ARCHITECTURE.md` 未明确标注 Native CLI (`sublift_cli` / `build/cpp/bin/sublift_cli` 或 `SubLift.app` 内置 CLI) 为全引擎支持的核心 Native 产品入口，也未清晰阐明 `uv run sublift` 是 Python 开发与 Oracle parity 回归/回滚工具。

### 1.2 目标 (Target State)
1. **支持 `--engine paddle` (允许 `--engine mock|vision|paddle`)**:
   - `sublift_cli` 参数解析解除限制，允许 `--engine mock|vision|paddle`。
   - 当指定 `--engine paddle` 时，`sublift_cli` 经由 fork/exec 启动 `sublift_worker --engine paddle`，并在 UDS IPC 上顺畅进行 Handshake 与 `StartJob` 编排。
2. **更新 CLI usage 帮助说明与身份输出**:
   - 从 `print_usage` 中移除 `"Paddle remains on the Python path"` 描述，明确标注 Native CLI 为支持 `mock|vision|paddle` 全引擎的 Native 产品入口。
   - 从 Handshake 回复的 `ByeMsg` 消息中提取 Worker 的 `runtime`、`engines` 和 `capabilities` 并在抽取前日志中输出，保证 Worker 身份信息一致透明。
3. **更新架构与用户文档**:
   - 更新 `README.md` 与 `docs/ARCHITECTURE.md`，明确 Native CLI (`sublift_cli` / `build/cpp/bin/sublift_cli` 或 `.app` 包内可执行文件) 为核心 Native 产品入口且支持全引擎；`uv run sublift` 是 Python 开发、调试与 Oracle parity 比对回归工具。
4. **绝对约束与非目标**:
   - **Out of scope**: 为 CLI 增加 in-process Paddle 推理捷径（CLI 必须 100% 走 Worker UDS IPC）；修改 macOS `.app` 包结构（归属于 feat-06910）。
   - **禁止改动**: CLI 架构（必须经由 Worker IPC），SRT 输出格式与比对 parity。

---

## 2. 有序实施步骤（每步可验证）

### Step 1: 修改 `cpp/src/cli/main.cpp` 支持 `--engine paddle`
- **操作**:
  - 修改 `cpp/src/cli/main.cpp` 中的 `parse_extract_args` 函数：
    - 允许 `out.engine` 为 `mock`, `vision`, `paddle` 之一。
    - 若指定其他非非法引擎，输出 `"Error: --engine must be mock|vision|paddle\n"` 并返回错误代码 2。
- **验证**:
  - 运行 `build/cpp/bin/sublift_cli extract --engine invalid`，确认输出合法引擎提示。
  - 运行 `build/cpp/bin/sublift_cli extract clip.mkv --engine paddle`（指定不可用视频时正常拦截 video not found，但不报引擎不支持错误）。

### Step 2: 更新 `print_usage` 帮助说明与选项列表
- **操作**:
  - 修改 `cpp/src/cli/main.cpp` 中的 `print_usage` 函数：
    - 移除 `"Paddle remains on the Python path (uv run sublift --engine paddle)."` 文本。
    - 将描述更新为 Native CLI 支持 `mock|vision|paddle` 全引擎。
    - 将选项说明更新为 `  --engine mock|vision|paddle  OCR engine (default: vision)`。
- **验证**:
  - 运行 `build/cpp/bin/sublift_cli --help` 或 `build/cpp/bin/sublift_cli extract --help`，确认帮助信息输出无误。

### Step 3: 从 Handshake `ByeMsg` 提取身份信息并打印
- **操作**:
  - 修改 `cpp/src/cli/main.cpp` 中的 Handshake 解析逻辑：
    - 在接收到 `ByeMsg` 后，解析出 `bye.runtime`、`bye.engines` 和 `bye.capabilities`。
    - 在 CLI 抽取前日志输出中，打印提取到的 Worker 身份元数据（如 `runtime` / `engines` / `capabilities` 概括），确保日志输出与 Worker 身份一致。
- **验证**:
  - 运行 `build/cpp/bin/sublift_cli extract clip.mkv --engine mock`，确认日志输出准确反映 `ByeMsg` 中 Worker 的身份和能力信息。

### Step 4: 更新 `README.md` 与 `docs/ARCHITECTURE.md`
- **操作**:
  - 更新 `README.md`：
    - 明确 `sublift_cli`（及二进制 `sublift`）为 Native 产品入口，支持 `mock|vision|paddle` 全部引擎。
    - 明确 `uv run sublift` 是 Python 开发与 Oracle parity 回归工具。
  - 更新 `docs/ARCHITECTURE.md`：
    - 补充 Native CLI 在 Phase 6.9 中的架构位置与全引擎 IPC 编排支持说明。
- **验证**:
  - 检查 Markdown 渲染与文本准确性。

### Step 5: 全日常门与功能验收 (`./init.sh`)
- **操作**:
  - 执行 `build/cpp/bin/sublift_cli extract --engine mock` 验证 CLI smoke。
  - 执行 `./init.sh`。
- **验证**:
  - `ruff` / `mypy` / `ctest` / `pytest` 及 cutover parity 检查全绿。

---

## 3. 要改 / 新建 / 移动的文件列表

### 新建文件
- `docs/cpp/plans/feat-06909-plan.md` (本计划文档)

### 修改文件
- `cpp/src/cli/main.cpp`
- `README.md`
- `docs/ARCHITECTURE.md`

---

## 4. 测试计划与验收命令

1. **CLI 参数与 `--engine paddle` / `mock` 功能验证**:
   ```bash
   build/cpp/bin/sublift_cli extract --help
   build/cpp/bin/sublift_cli extract --engine mock
   ```
2. **日常全门验证 (Baseline Gate)**:
   ```bash
   ./init.sh
   ```

---

## 5. 风险与回滚

- **风险**: 当本地没有配置/准备 Paddle 模型时，运行 `--engine paddle` 可能会导致 Worker 抛出资源缺失错误。
- **规避**: Worker 已拥有强类型 `ResourceLocator` 异常处理机制，CLI 会捕获 DoneMsg/ErrorMsg 中的可读错误提示（如缺少 Paddle 模型文件与指引），不崩溃且优雅退出。
- **回滚方案**: `git reset --hard HEAD`

---

## 6. 非目标与约束再确认 (Non-Goals)

1. **绝对禁止** 为 CLI 增加 in-process Paddle 推理捷径（CLI 必须 100% 走 Worker UDS IPC）。
2. **不在此任务** 中修改 `.app` 打包逻辑（属于 feat-06910）。
3. **严禁改动** CLI 架构（必须经由 Worker IPC）。
4. **严禁改动** SRT 输出格式与比对 parity。

---

## 7. Done 定义

- [ ] `cpp/src/cli/main.cpp` 参数解析支持 `--engine mock|vision|paddle`。
- [ ] 当 `--engine paddle` 时，`sublift_cli` 能正确 fork/exec 启动 `sublift_worker --engine paddle` 并经 IPC 完成抽帧与识别。
- [ ] `print_usage` 更新完成，移除过时短语，标明 Native CLI 支持全引擎。
- [ ] 提取前日志成功输出 Handshake `ByeMsg` 中解析出的 Worker 身份元数据。
- [ ] `README.md` 与 `docs/ARCHITECTURE.md` 更新完成，明确 Native CLI 为主产品入口，`uv run sublift` 为 Python dev/oracle 工具。
- [ ] CLI mock/paddle 验收命令验证通过。
- [ ] `./init.sh` 全门 100% PASS。
