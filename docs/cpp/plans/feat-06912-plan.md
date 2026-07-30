# feat-06912 可执行计划：Python-free Standalone Native 运行验证

- **Feature ID**: feat-06912
- **Feature Name**: Python-free Standalone Native 运行验证 (Python-free 产品门)
- **Phase**: Phase 6.9 (Native 产品架构整理 / 去 Python 分发)
- **Worktree**: `/Volumes/lab/pp/SubLift_CPP`
- **关联设计**: `docs/cpp/phase6.9-native-product-architecture.md` / `docs/cpp/phase6.9-implementation-plan.md`

---

## 1. 现状与目标 Diff

### 1.1 现状 (Current State)
1. **缺少 Python-free 隔离运行校验门 (`scripts/check_python_free.py`)**:
   - 目前尚无专门的 Python-free 运行门脚本，用于在绝对干净的环境变量 (`env -i` 擦除 `PYTHONHOME`, `PYTHONPATH`, `VIRTUAL_ENV`, `PATH` 仅保留系统基础 `PATH` `/usr/bin:/bin`) 下测试 Native 二进制 (`build/cpp/bin/sublift_cli` 以及 `build/SubLift.app/Contents/MacOS/sublift_cli`)。
2. **`check_app_bundle.py --strict` 模式需要完备支持机械与路径绑定断言**:
   - `scripts/check_app_bundle.py` 已支持 `--strict` 检查 `otool -L` 链接外部 Homebrew/venv 路径，但需进一步确保 `check_app_bundle.py --strict` 能够严格断言 App Bundle 以及 `sublift_cli` / `sublift_worker` 无任何 Python/venv/Homebrew 绝对硬路径绑定。
3. **校验门未接入日常测试套件或 CI 门禁**:
   - 现有的 `check_app_bundle.py` 和新增的 `check_python_free.py` 尚未在自动化测试流程与验收脚本中统一串联收口。

### 1.2 目标 (Target State)
1. **编写 `scripts/check_python_free.py` 隔离运行校验门脚本**:
   - 在完全清除 `PYTHONHOME`, `PYTHONPATH`, `VIRTUAL_ENV`, `PATH` (仅留系统基础 PATH `/usr/bin:/bin`) 的干净隔离环境变量下 (`env -i`) 运行 `build/cpp/bin/sublift_cli` 以及 `build/SubLift.app/Contents/MacOS/sublift_cli`。
   - 验证 `sublift_cli extract --engine mock` 在零 Python 环境依赖下成功运行、输出正确 JSON/progress、并正常生成 SRT 文件。
   - 包含进程树断言 (process tree assertion)，确保在运行 Native 二进制期间无任何 `python` 或 `uv` 子进程被拉起。
2. **增强 `check_app_bundle.py --strict` 机械与绝对硬路径绑定断言**:
   - 使用 `check_app_bundle.py --strict` 验证 `SubLift.app` 及 `sublift_cli` / `sublift_worker` 无任何 Python / `.venv` / Homebrew 绝对硬路径绑定。
3. **Python-free 校验集接入 Gate/测试套件**:
   - 确保 `python3 scripts/check_python_free.py` 与 `python3 scripts/check_app_bundle.py` 可稳定独立运行与重复验证。
4. **全日常门与产品契约无退化**:
   - 执行 `./init.sh` 确保系统总体门全绿。

### 1.3 绝对约束与非目标
- **In Scope**:
  1. 编写或增强 Python-free 隔离运行校验门 (`scripts/check_python_free.py`)：在干净隔离环境变量下 (`env -i`) 运行 `build/cpp/bin/sublift_cli` 与 `build/SubLift.app/Contents/MacOS/sublift_cli`，验证 `extract --engine mock` 成功生成 SRT 且零 Python 依赖与零 Python/uv 子进程。
  2. 使用 `check_app_bundle.py --strict` 验证 App Bundle 及 `sublift_cli` 无任何 Python/venv/Homebrew 绝对硬路径绑定。
  3. 将 Python-free 校验集接入测试或 gate。
- **Out of Scope**:
  1. 彻底从代码库中删除 Python 回滚引擎（归属 `feat-06913`）。
- **绝对禁止**:
  - 改动算法、golden、IPC 协议与底层 C++ 静态 Target 依赖。

---

## 2. 有序实施步骤（每步可验证）

### Step 1: 编写 `scripts/check_python_free.py` 隔离运行校验门
- **操作**:
  - 创建 `scripts/check_python_free.py` 校验脚本。
  - 自动定位 `build/cpp/bin/sublift_cli` 以及 App Bundle 中的 `build/SubLift.app/Contents/MacOS/sublift_cli`（若 Bundle 存在）。
  - 使用 `env -i` 构造干净隔离环境变量（仅保留系统基础 `PATH=/usr/bin:/bin`），执行 `sublift_cli extract --engine mock -i <test_video> -o <out_srt>`。
  - 校验命令返回值 0、验证输出 SRT 内容格式合法，并在子进程运行时/运行后断言未拉起任何 python/uv 子进程。
- **验证**:
  - 运行 `python3 scripts/check_python_free.py`，确认返回 0 且 PASS。

### Step 2: 增强 `scripts/check_app_bundle.py` `--strict` 检查
- **操作**:
  - 检查并完善 `scripts/check_app_bundle.py` 中的 `--strict` 断言，验证 `SubLift.app` 内可执行文件 (`sublift_cli`, `sublift_worker`) 无任何 `/opt/homebrew`, `/usr/local`, `.venv` 或 Python 动态库/硬路径绑定。
- **验证**:
  - 运行 `python3 scripts/check_app_bundle.py --strict`，确认 PASS。

### Step 3: App Bundle 打包与 Python-free 综合校验
- **操作**:
  - 运行 Bundle 打包 `bash scripts/build_app_bundle.sh --no-build`。
  - 运行 `python3 scripts/check_app_bundle.py --strict` 和 `python3 scripts/check_python_free.py`。
- **验证**:
  - 输出打包完成与两个 Python-free 校验脚本全 PASS 信息。

### Step 4: 全日常门验证 (`./init.sh`)
- **操作**:
  - 执行 `./init.sh`。
- **验证**:
  - `ruff` / `mypy` / `ctest` / `pytest` 及 cutover parity 检查全绿。

---

## 3. 要改 / 新建 / 移动的文件列表

### 新建文件
- `docs/cpp/plans/feat-06912-plan.md` (本计划文档)
- `scripts/check_python_free.py` (Python-free 隔离运行校验门脚本)

### 修改文件
- `scripts/check_app_bundle.py` (增强 `--strict` 校验与路径绑定断言)

---

## 4. 测试计划与验收命令

1. **Python-free 隔离运行校验**:
   ```bash
   python3 scripts/check_python_free.py
   ```
2. **App Bundle `--strict` 机械与绑定校验**:
   ```bash
   python3 scripts/check_app_bundle.py --strict
   ```
3. **日常全门验证 (Baseline Gate)**:
   ```bash
   ./init.sh
   ```

---

## 5. 风险与回滚

- **风险**:
  1. 系统极简 `PATH` (`/usr/bin:/bin`) 下由于缺少环境依赖导致 `sublift_cli` 无法正常 spawn 运行。
     - **规避**: mock 引擎提取不需要外部 ffmpeg，对于 Standard App Bundle，Worker 会优先使用 `Contents/Resources/bin/` 下随包放置的二进制/资源，且系统的基本 Unix 工具链均位于 `/usr/bin:/bin`。
  2. 测试视频依赖问题。
     - **规避**: 在 `scripts/check_python_free.py` 中使用内建合成短视频/通用测试夹具，确保脚本自洽可独立复现运行。
- **回滚方案**: `git reset --hard HEAD`

---

## 6. 非目标与约束再确认 (Non-Goals)

1. **绝对禁止** 在 `feat-06912` 中从代码库中彻底删除 Python 回滚引擎（属于 `feat-06913`）。
2. **绝对禁止** 改动算法、golden、IPC 协议与底层 C++ 静态 Target 依赖。

---

## 7. Done 定义

- [ ] 编写 `scripts/check_python_free.py`，支持在 `env -i` 干净隔离环境变量下运行 `build/cpp/bin/sublift_cli` 以及 `build/SubLift.app/Contents/MacOS/sublift_cli`。
- [ ] 验证 `sublift_cli extract --engine mock` 在零 Python 环境依赖下成功运行并生成 SRT。
- [ ] 进程树断言确保运行全过程零 `python`/`uv` 子进程拉起。
- [ ] `check_app_bundle.py --strict` 验证 App Bundle 及 `sublift_cli` 无任何 Python/venv/Homebrew 绝对硬路径绑定。
- [ ] 验收命令 `python3 scripts/check_python_free.py` 执行 PASS。
- [ ] 验收命令 `python3 scripts/check_app_bundle.py` (含 `--strict`) 执行 PASS。
- [ ] 验收命令 `./init.sh` 100% PASS。
