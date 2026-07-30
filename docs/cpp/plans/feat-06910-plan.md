# feat-06910 可执行计划：macOS Product Bundle 布局

- **Feature ID**: feat-06910
- **Feature Name**: macOS Product Bundle 布局
- **Phase**: Phase 6.9 (Native 产品架构整理)
- **Worktree**: `/Volumes/lab/pp/SubLift_CPP`
- **关联设计**: `docs/cpp/phase6.9-native-product-architecture.md` / `docs/cpp/phase6.9-implementation-plan.md`

---

## 1. 现状与目标 Diff

### 1.1 现状 (Current State)
1. **`scripts/build_app_bundle.sh` 缺少 `--no-build` 参数控制**:
   - 脚本中默认包含 `cmake -B ...` 和 `cmake --build ...` 逻辑。当由 CMake `add_custom_target(bundle ...)` 触发时，会导致二次/递归 C++ 构建。
2. **拷贝逻辑存在空 glob 报错风险**:
   - 脚本中使用 `cp -R "${MODEL_CACHE}/"* ...` 或 `cp "${ORT_LIB}"* ...`，当目标路径下无匹配文件或未开启 `nullglob` 时，Bash 会抛出空 glob 报错。
3. **二进制缺失相对 RPATH 注入**:
   - 未对打包后的可执行文件 `SubLift.app/Contents/MacOS/sublift_cli` 和 `SubLift.app/Contents/Helpers/sublift_worker` 使用 `install_name_tool -add_rpath "@executable_path/../Frameworks"` 注入相对 RPATH，导致在加载 `Contents/Frameworks/` 下的动态库（如 ONNXRuntime）时可能会找不到依赖。
4. **`cpp/CMakeLists.txt` 的 custom target 尚未传递 `--no-build`**:
   - `add_custom_target(bundle ...)` 目前直接调用打包脚本，没有带 `--no-build` 标记。
5. **`scripts/check_app_bundle.py` 校验覆盖度尚需增强**:
   - 需进一步确保完备校验 Standard Bundle 物理目录树（6大必需路径）、可执行权限 (`+x`)、薄 CLI 独立性（严禁强链接 ONNXRuntime/OpenCV/Vision 以及 Homebrew/venv 路径）以及 `otool -L` / RPATH 检查。

### 1.2 目标 (Target State)
1. **建立规范的 macOS Standard Application Bundle (`SubLift.app`) 目录结构**:
   - `SubLift.app/Contents/MacOS/sublift_cli` (薄 CLI 入口)
   - `SubLift.app/Contents/Helpers/sublift_worker` (Backend Task Worker)
   - `SubLift.app/Contents/Resources/models/` (模型文件)
   - `SubLift.app/Contents/Resources/bin/` (内置依赖如 ffmpeg)
   - `SubLift.app/Contents/Frameworks/` (动态库如 ONNXRuntime)
   - `SubLift.app/Contents/Info.plist` (Bundle 元数据)
2. **完善 `scripts/build_app_bundle.sh` 脚本**:
   - 支持 `--no-build` 参数：当传入该选项时跳过 CMake 配置与构建步骤，专门供 CMake `bundle` custom target 使用。
   - 对 `sublift_cli` 与 `sublift_worker` 使用 `install_name_tool -add_rpath "@executable_path/../Frameworks"` 注入相对 RPATH（幂等容错处理）。
   - 优化文件拷贝逻辑，开启 `shopt -s nullglob` 或显式检查文件列表，防止空 glob 匹配报错。
3. **完善 `cpp/CMakeLists.txt` 中的 `bundle` custom target**:
   - 将命令配置为 `"${CMAKE_SOURCE_DIR}/../scripts/build_app_bundle.sh" --no-build`，`DEPENDS sublift_cli sublift_worker`。
4. **完善 `scripts/check_app_bundle.py` 机械校验工具**:
   - 校验 Bundle 6 大核心物理目录树及 `chmod +x` 可执行权限。
   - 薄 CLI 极纯独立性校验：`otool -L` 中严禁出现 `onnxruntime`、`opencv`、`Vision.framework` 以及 Homebrew / .venv 绝对路径。
   - `sublift_worker` 链接校验与 relative RPATH 检查。

### 1.3 绝对约束与非目标
- **Out of Scope**: 开发者证书 (Developer ID) 签名与公证 (属于 feat-06911)。
- **禁止改动**: Bundle 内部物理相对目录结构；Worker UDS 通信逻辑。

---

## 2. 有序实施步骤（每步可验证）

### Step 1: 完善 `scripts/build_app_bundle.sh`
- **操作**:
  - 在 `scripts/build_app_bundle.sh` 解析命令行参数，增加 `--no-build` 选项，当为 1 时跳过 CMake configure 与 build。
  - 在拷贝二进制文件后，对 `Contents/MacOS/sublift_cli` 与 `Contents/Helpers/sublift_worker` 执行：
    `install_name_tool -add_rpath "@executable_path/../Frameworks" <binary> 2>/dev/null || true` 注入相对 RPATH。
  - 在模型、ffmpeg 与 Frameworks 拷贝前使用 `shopt -s nullglob` 或判断通配符匹配结果，防止空 glob 匹配抛错。
- **验证**:
  - 运行 `bash scripts/build_app_bundle.sh`，检查生成 `build/SubLift.app` 及终端日志无报错。

### Step 2: 修改 `cpp/CMakeLists.txt` 的 `bundle` custom target
- **操作**:
  - 修改 `cpp/CMakeLists.txt` 结尾的 `add_custom_target(bundle ...)`，将其 COMMAND 更新为：
    `COMMAND "${CMAKE_SOURCE_DIR}/../scripts/build_app_bundle.sh" --no-build`
- **验证**:
  - 运行 `cmake --build build/cpp --target bundle`，确认 CMake 不会无限递归调用 C++ 编译，并顺利完成打包。

### Step 3: 完善 `scripts/check_app_bundle.py` 机械校验逻辑
- **操作**:
  - 检查并补齐 6 大必需路径校验 (`Info.plist`, `MacOS/sublift_cli`, `Helpers/sublift_worker`, `Resources/models`, `Resources/bin`, `Frameworks`)。
  - 校验 `sublift_cli` 与 `sublift_worker` 的 `os.X_OK` 权限。
  - 校验 `sublift_cli` 的 `otool -L` 极纯度（不允许 ONNXRuntime / OpenCV / Vision / Brew / venv）。
  - 校验二进制依赖项及 RPATH 中包含相对路径。
- **验证**:
  - 运行 `python3 scripts/check_app_bundle.py`，确认返回 0 且打印 `[OK] App Bundle mechanical layout verification PASS`。

### Step 4: 运行 Bundle 构建与机械校验
- **操作**:
  - 依次执行 `bash scripts/build_app_bundle.sh` 与 `python3 scripts/check_app_bundle.py`。
- **验证**:
  - 输出打包完成与校验 PASS 信息。

### Step 5: 全日常门与功能验收 (`./init.sh`)
- **操作**:
  - 执行 `./init.sh`。
- **验证**:
  - `ruff` / `mypy` / `ctest` / `pytest` 及 cutover parity 检查全绿。

---

## 3. 要改 / 新建 / 移动的文件列表

### 新建文件
- `docs/cpp/plans/feat-06910-plan.md` (本计划文档)

### 修改文件
- `scripts/build_app_bundle.sh`
- `cpp/CMakeLists.txt`
- `scripts/check_app_bundle.py`

---

## 4. 测试计划与验收命令

1. **打包脚本构建验证**:
   ```bash
   bash scripts/build_app_bundle.sh
   ```
2. **App Bundle 机械校验验证**:
   ```bash
   python3 scripts/check_app_bundle.py
   ```
3. **CMake Target 验证**:
   ```bash
   cmake --build build/cpp --target bundle
   ```
4. **日常全门验证 (Baseline Gate)**:
   ```bash
   ./init.sh
   ```

---

## 5. 风险与回滚

- **风险**:
  1. `install_name_tool -add_rpath` 重复运行时若该 rpath 已存在会返回非零退出码。
     - **规避**: 在命令后追加 `2>/dev/null || true` 或先通过 `otool -l` 检查是否存在该 LC_RPATH。
  2. 源环境中无 Cached 模型或 ffmpeg 库，拷贝通配符报文件未找到错误。
     - **规避**: 采用 `shopt -s nullglob` 或显式 `compgen` / `if` 判断后再执行拷贝。
- **回滚方案**: `git reset --hard HEAD`

---

## 6. 非目标与约束再确认 (Non-Goals)

1. **绝对禁止** 在 `feat-06910` 中引入 Developer ID 证书签名或 Apple 公证逻辑（属于 `feat-06911`）。
2. **绝对禁止** 修改 Bundle 内部物理相对目录结构（必须保持 `Contents/MacOS/sublift_cli`, `Contents/Helpers/sublift_worker`, `Contents/Resources/models/`, `Contents/Resources/bin/`, `Contents/Frameworks/`, `Contents/Info.plist`）。
3. **绝对禁止** 修改 Worker UDS 通信逻辑。

---

## 7. Done 定义

- [ ] Bundle 包含规范的 6 大标准路径（`MacOS/sublift_cli`, `Helpers/sublift_worker`, `Resources/models/`, `Resources/bin/`, `Frameworks/`, `Info.plist`）。
- [ ] `scripts/build_app_bundle.sh` 支持 `--no-build` 参数，调用时不重复编译 C++ 工程。
- [ ] `scripts/build_app_bundle.sh` 使用 `install_name_tool` 为 `sublift_worker` 和 `sublift_cli` 注入 `@executable_path/../Frameworks` RPATH。
- [ ] `scripts/build_app_bundle.sh` 优化文件拷贝逻辑，防止空 glob 匹配报错。
- [ ] `cpp/CMakeLists.txt` 中的 `bundle` target 已添加 `--no-build` 参数。
- [ ] `scripts/check_app_bundle.py` 增强校验逻辑，覆盖物理目录树、可执行权限、薄 CLI 独立性与 `otool -L` / RPATH。
- [ ] 验收命令 `bash scripts/build_app_bundle.sh` 执行 PASS。
- [ ] 验收命令 `python3 scripts/check_app_bundle.py` 执行 PASS。
- [ ] 验收命令 `./init.sh` 100% PASS。
