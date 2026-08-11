# AGENTS.md

> SubLift 的项目级 Agent 契约：只放稳定事实、可复制命令与长期边界。流程细节在 `.agent/` 中按需读取，不整体导入；当前 Phase 只由 `phases.json` 与 `progress.md` 声明。

## 项目概述

- **项目名**：`SubLift`
- **目标**：本地提取视频中烧录的硬字幕，输出可编辑的 SRT（ASS/VTT 后续扩展）。
- **技术栈**：Python 3.12+（`uv`）、C++20 / ObjC++（CMake + Ninja）、Swift 5.9+（SwiftPM）、ffmpeg；macOS 13+ 的 Vision 可选，Paddle 使用 ONNX Runtime / OpenCV。
- **进度真源**：`phases.json` + `progress.md`。`feature-list.json` 仅为历史兼容索引。

## 权威信息源

不确定时先查下表；仍不确定则问用户，不要猜。

| 问题 | 先查 |
|---|---|
| 架构 / 模块设计 | `docs/ARCHITECTURE.md`、`docs/design/*.md`、`docs/cpp/` |
| 需求 / 范围 / 验收 | `docs/REQUIREMENTS.md` |
| 历史技术决策 | `docs/DECISIONS.md` |
| 踩过的坑 | `docs/HURDLES.md` |
| 当前该做什么 | `phases.json` → 对应 Phase 的 `detail_file` |
| 历史功能组或旧链接 | `feature-list.json`（只读兼容查询） |

## 项目结构地图

- `src/sublift/`：Python Oracle、CLI、benchmark 与 IPC 兼容运行时。
- `cpp/`：Native C++ Core、Worker、Vision/Paddle adapters 与 CTest。
- `apps/macos/`：SwiftUI 开发者 GUI 与 Swift 测试。
- `benchmark/`：版本化 configs、datasets、baselines 与 parity 资产；本机运行产物写 `debug/benchmark/`。
- `.agent/`：轻量项目规则、会话入口与提交辅助；已归档的复杂能力编排不属于当前活跃 Harness。

## 环境与标准命令

**安装 / 运行 / 构建**：

- 安装产品 extras：`uv sync --extra vision --extra paddle`
- 会话初始化：`./init.sh`
- 标准开发验证：`./scripts/verify-standard.sh`
- Native Debug 构建：`cmake -S cpp -B build/cpp -G Ninja -DCMAKE_BUILD_TYPE=Debug -DSUBLIFT_REQUIRE_OPENCV=ON -DSUBLIFT_ENABLE_VISION=ON && cmake --build build/cpp`
- macOS GUI：`cd apps/macos && swift build && swift run SubLiftMac`

**测试与验证**：

- Python lint：`uv run ruff check .`
- Python 类型检查：`uv run mypy src tests`
- Python 日常测试：`uv run pytest -m "not integration" --no-cov`
- C++：`ctest --test-dir build/cpp --output-on-failure`
- Swift（macOS）：`cd apps/macos && swift test`
- cutover parity：`uv run python scripts/parity/check_cutover_gate.py --check --skip-runtime --skip-gt --report-out /tmp/sublift_cutover_gate.md`
- 发布 / 完整 cutover（外部资源齐备时）：`SUBLIFT_VERIFY_RUNTIME=1 SUBLIFT_VERIFY_REQUIRE_GT=1 ./scripts/verify-standard.sh`

`./init.sh` 只建立开工前提；依赖同步、lint、类型检查、构建、测试与 parity 均由独立命令或 `scripts/verify-standard.sh` 承担。

## 代码 / 架构约束

- Python formatting/lint 见 `pyproject.toml` 的 Ruff 配置，类型检查使用 strict mypy；C++ / Swift 遵循现有 target 与目录边界。
- 产品模块保持 `extractor / detector / ocr / pipeline / export` 的 Protocol 分层；Native target 依赖方向以 `docs/cpp/` 契约为准。
- 产品默认 C++ runtime 必须 fail-closed；Python 仅在显式 `--runtime python` / `SUBLIFT_RUNTIME=python` 下作为 Oracle 或回滚。

  ```python
  if runtime == "python":
      return run_python_oracle(...)
  return run_cpp_worker_or_raise(...)
  ```

- 活样本：`src/sublift/benchmark/config.py`（严格类型、显式错误与路径解析）；`cpp/src/application/`（Native composition root）。

**Git**：

- 遵循仓库现有分支策略，不擅自切换或重写历史。
- Commit message 使用 Conventional Commits，并经 `.agent/skills/commit/SKILL.md` 起草与执行。
- **提交 / 推送必须先获得用户明确同意。**

## Always / Ask First / Never

- **Always**
  - 一次只修改一个已登记的当前 Feature；历史 Phase 只做明确的兼容迁移。
  - 开工与交接时运行 `./init.sh`，交付前运行与改动范围匹配的真实验证。
  - 将最终验证证据写入对应 Phase feature 的 `evidence`，保持 `progress.md` 为简短导航。
  - 新 Feature 使用五位 ID，并通过 `docs/phases/phaseN.schema.json` 的 canonical 分支。
- **Ask first**
  - 新增第三方依赖、数据库或破坏性 schema 变更、CI/CD、发布、签名、公证。
  - 修改本文件或 `.agent/` 下的规则与 skill。
  - commit、push、删除历史 evidence、既有 adapter、本地测试素材或分支。
- **Never**
  - 绕过 `.agent/skills/commit/` 直接提交或推送。
  - 将 `feature-list.json` 重新当作进度真源，或伪造已归档能力包的 state / review / receipt。
  - 将 C++ parity/golden harness 与 Agent Harness 混为同一概念。

## Definition of Done

同时满足才算完成：

- [ ] 目标行为或文档目标已实现
- [ ] 与改动范围匹配的验证已实际运行
- [ ] 最终验证证据已写入对应 Phase feature 的 `evidence`
- [ ] `./init.sh` 可通过

## Harness 导航

| 时机 | 入口 |
|---|---|
| 开始任何工作前 | `.agent/skills/session-bootstrap/SKILL.md` |
| 结束会话前 | `.agent/skills/session-handoff/SKILL.md` |
| 需要提交或推送时 | `.agent/skills/commit/SKILL.md`（仍须先获用户同意） |
| 进度 / Phase 文件约定 | `.agent/rules/project-continuity.md` |
| 初始化职责与扩展边界 | `.agent/rules/initialization.md` |

Planning、Testing、Review、Verification 与 Feature/Phase 编排已从活跃 Harness 移出；未来仅在项目真实需要时增量引入，不以历史文件或聊天记忆模拟其状态链。
