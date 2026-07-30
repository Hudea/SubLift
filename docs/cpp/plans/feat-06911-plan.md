# feat-06911 可执行计划：Swift App Shell + Native Driver

- **Feature ID**: feat-06911
- **Feature Name**: Swift App Shell + Native Driver (签名、公证与干净机 Gatekeeper 对接)
- **Phase**: Phase 6.9 (Native 产品架构整理)
- **Worktree**: `/Volumes/lab/pp/SubLift_CPP`
- **关联设计**: `docs/cpp/phase6.9-native-product-architecture.md` / `docs/cpp/phase6.9-implementation-plan.md`

---

## 1. 现状与目标 Diff

### 1.1 现状 (Current State)
1. **`PipelineClient.swift` Helper 路径查找需精准对齐 Standard Bundle 布局**:
   - 目前 `PipelineClient.findWorkerExecutable` 查找 App Bundle 资源时优先检查了 `mainBundle.resourceURL` 下的相对路径 (`Contents/Resources/Helpers/sublift_worker`) 和 `Contents/MacOS/sublift_worker`。
   - 在 feat-06910 确立的 macOS Standard Bundle 规范中，Native Worker 的物理位置为 `SubLift.app/Contents/Helpers/sublift_worker`。因此需要显式将 `mainBundle.bundlePath + "/Contents/Helpers/sublift_worker"` 纳入最高优先级的 App Bundle 检索路径。
2. **Swift 单元测试完备性**:
   - `apps/macos` 下已有 139 个 Swift 单元测试用例，覆盖编解码、IPC 通信、进程生命周期及路由解析，需要确保在补齐路由逻辑后 139 用例全部 PASS。
3. **Apple Developer 证书与公证环境约束**:
   - 本地开发机及无付费开发者账号的环境缺少 Developer ID 签名证书与 Apple 云端公证 (Notarization) 凭证。
   - 依照 Phase 6.9 规范，无法进行线上 `xcrun notarytool submit` 时，需将公证标注为外部依赖/降级运行模式，同时通过 ad-hoc 签名 (`codesign --sign -`) 与属性清理保证未签名 Bundle 在本地开发环境中可正常降级运行，不阻塞功能研发与自动化测试。

### 1.2 目标 (Target State)
1. **精准路由指向 Standard App Bundle 路径**:
   - `apps/macos/Sources/SubLiftMac/Core/PipelineClient.swift` 增加对 `SubLift.app/Contents/Helpers/sublift_worker` 物理路径的优先检索。
2. **Swift 单元测试集全数通过**:
   - 验证 `cd apps/macos && swift test`，确保 139 个单元测试用例 100% 绿码。
3. **规范化证书缺失约束与本地降级运行机制**:
   - 明确 Apple Developer 证书/公证在无证书环境下的处理机制：
     - **生产环境 (带证书)**：`codesign` 强签名 + `xcrun notarytool` 公证 + `stapler` 票据附着。
     - **本地/CI 环境 (无证书)**：采用 ad-hoc 签名 `codesign --force --deep -s - SubLift.app`，标注公证状态为 `blocked/degraded (External Dependency missing)`，保证未签名 Bundle 本地降级可运行。
4. **全日常门与产品契约无退化**:
   - 执行 `./init.sh` 确保系统总体门全绿。

### 1.3 绝对约束与非目标
- **Out of Scope**: 物理连接 Apple 开发者付费账号进行云端公证。
- **绝对禁止**: 改动 Swift UDS IPC 协议与状态机契约（保持 `hello`/`bye`/`push_entry`/`progress`/`done` 消息结构与分帧逻辑不变）。

---

## 2. 有序实施步骤（每步可验证）

### Step 1: 补齐 `PipelineClient.swift` Helper 物理路径查找
- **操作**:
  - 在 `apps/macos/Sources/SubLiftMac/Core/PipelineClient.swift` 的 `findWorkerExecutable` 方法中，在检索 `resourceURL` 之前，插入 `mainBundle.bundlePath + "/Contents/Helpers/sublift_worker"` 的路径检查。
  - 在 `apps/macos/Tests/SubLiftMacTests/PipelineClientTests.swift` 中增加/更新单测，验证包含 `Contents/Helpers/sublift_worker` 的路径检索行为。
- **验证**:
  - 运行 `cd apps/macos && swift test`，确认单测通过。

### Step 2: 验证 Swift 单元测试集 (139 用例)
- **操作**:
  - 在 `apps/macos` 目录下运行全量 Swift 单元测试。
- **验证**:
  - 执行 `cd apps/macos && swift test`
  - 确认输出 `Executed 139 tests, with 0 failures` (或无任何失败)。

### Step 3: 完善签名、公证 Runbook 与本地降级文档
- **操作**:
  - 在计划与相关文档中明确 codesign / notarize / staple 命令行规范以及无证书环境下的降级策略。
  - ad-hoc 本地签名命令：`codesign --force --deep --options runtime -s - build/SubLift.app`
  - 验证命令：`codesign --verify --deep --strict build/SubLift.app`
- **验证**:
  - 对打包后的 `build/SubLift.app` 执行 ad-hoc 签名与 strict verify 检查。

### Step 4: 机械 Bundle 校验与整合
- **操作**:
  - 运行 Bundle 打包与机械校验脚本。
- **验证**:
  - 运行 `bash scripts/build_app_bundle.sh --no-build`
  - 运行 `python3 scripts/check_app_bundle.py`

### Step 5: 全日常门验证 (`./init.sh`)
- **操作**:
  - 执行 `./init.sh`。
- **验证**:
  - `ruff` / `mypy` / `ctest` / `pytest` 及 cutover parity 检查全绿。

---

## 3. 要改 / 新建 / 移动的文件列表

### 新建文件
- `docs/cpp/plans/feat-06911-plan.md` (本计划文档)

### 修改文件
- `apps/macos/Sources/SubLiftMac/Core/PipelineClient.swift`
- `apps/macos/Tests/SubLiftMacTests/PipelineClientTests.swift`

---

## 4. 测试计划与验收命令

1. **Swift 单元测试验收**:
   ```bash
   cd apps/macos && swift test
   ```
2. **App Bundle 机械校验**:
   ```bash
   python3 scripts/check_app_bundle.py
   ```
3. **日常全门验证 (Baseline Gate)**:
   ```bash
   ./init.sh
   ```

---

## 5. 风险与回滚

- **风险**:
  1. 无 Apple Developer ID 证书时 macOS Gatekeeper 对移至新机器的未签名 `.app` 抛出隔离警告。
     - **规避**: 在开发与 CI 测试阶段使用 `codesign -s -` ad-hoc 签名及 `xattr -d com.apple.quarantine` 解除隔离标记；在 Phase 跟踪中将云端公证记录为 External Dependency。
  2. Swift 测试执行依赖 CMake 构建产物中的 `sublift_worker` 二进制。
     - **规避**: 测试运行前若需要真实 IPC 交互，确保已先通过 `cmake --build build/cpp` 编译生成 `sublift_worker`。
- **回滚方案**: `git reset --hard HEAD`

---

## 6. 非目标与约束再确认 (Non-Goals)

1. **绝对禁止** 改动 Swift UDS IPC 协议与状态机契约（如包头 4 字节大端无符号整数前缀、JSON 消息载荷结构等）。
2. **绝对禁止** 物理强行要求连接 Apple 开发者付费账号进行云端公证（若环境无证书则降级处理并记录说明）。

---

## 7. Done 定义

- [ ] `PipelineClient.swift` 确认优先路由指向 `SubLift.app/Contents/Helpers/sublift_worker`。
- [ ] `cd apps/macos && swift test` 139 个单元测试用例全数通过。
- [ ] 阐明 Apple Developer 证书/公证缺失时的处理规范，支持 ad-hoc 签名与未签名 Bundle 本地降级运行。
- [ ] 验收命令 `cd apps/macos && swift test` 执行 PASS。
- [ ] 验收命令 `./init.sh` 执行 100% PASS。
