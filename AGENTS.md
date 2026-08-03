# AGENTS.md

> SubLift 的项目级 Agent 契约：这里只放稳定事实、命令与边界；可恢复的执行流程由 `.agent/` 提供。

## 项目概述

- **项目名**：`SubLift`
- **目标**：本地提取视频中烧录的硬字幕，输出可编辑的 SRT（ASS/VTT 后续扩展）。
- **技术栈**：Python 3.12+（`uv`）、C++20 / ObjC++（CMake + Ninja）、Swift 5.9+（SwiftPM）、ffmpeg；macOS 13+ 的 Vision 可选，Paddle 使用 ONNX Runtime / OpenCV。
- **进度真源**：`phases.json` + `progress.md`。根 `feature-list.json` 是历史兼容索引，不能作为当前任务选择或状态更新的真源。

## 权威信息源

不确定时先查下表；仍不确定则问用户，不要猜。

| 问题 | 先查 |
|---|---|
| 架构 / 模块设计 | `docs/ARCHITECTURE.md`、`docs/design/*.md`、`docs/cpp/` |
| 需求 / 范围 / 验收 | `docs/REQUIREMENTS.md` |
| 历史技术决策 | `docs/DECISIONS.md` |
| 踩过的坑 | `docs/HURDLES.md` |
| 当前该做什么 | `phases.json` → 对应 Phase 的 `detail_file` |
| 历史功能组或旧链接 | `feature-list.json`（仅兼容查询） |

## 项目结构地图

- `src/sublift/`：Python Oracle、CLI、benchmark 与 IPC 兼容运行时。
- `cpp/`：Native C++ Core、Worker、Vision/Paddle adapters 与 CTest。
- `apps/macos/`：SwiftUI 开发者 GUI 与 Swift 测试。
- `benchmark/`：版本化 configs、datasets、baselines 与 parity fixtures；本机产物写 `debug/benchmark/`。
- `docs/`：架构、需求、Phase 细节、计划、决策与障碍记录。
- `.agent/`：Harness rules、skills、schemas、validators 与可恢复 state（state 不入库）。

## 环境与标准命令

**安装 / 运行 / 构建**：

- 安装产品 extras：`uv sync --extra vision --extra paddle`
- 会话 L0：`./init.sh`（只检查 Harness 文件、JSON 与 Phase index，不安装/构建/测试）
- 标准开发验证：`./scripts/verify-standard.sh`
- Native Debug 构建：`cmake -S cpp -B build/cpp -G Ninja -DCMAKE_BUILD_TYPE=Debug -DSUBLIFT_REQUIRE_OPENCV=ON -DSUBLIFT_ENABLE_VISION=ON && cmake --build build/cpp`
- macOS GUI：`cd apps/macos && swift build && swift run SubLiftMac`

**测试与验证**：

- Python lint：`uv run ruff check .`
- Python 类型检查：`uv run mypy src tests`
- Python 日常测试：`uv run pytest -m "not integration" --no-cov`
- C++：`ctest --test-dir build/cpp --output-on-failure`
- cutover parity：`uv run python scripts/parity/check_cutover_gate.py --check --skip-runtime --skip-gt --report-out /tmp/sublift_cutover_gate.md`
- Swift（macOS）：`cd apps/macos && swift test`
- 发布 / 完整 cutover（外部资源齐备时）：`SUBLIFT_INIT_RUNTIME=1 SUBLIFT_INIT_REQUIRE_GT=1 ./scripts/verify-standard.sh`

`scripts/verify-standard.sh` 继承旧日常门：一次依赖同步、ruff、mypy、C++ configure/build/ctest、单次非集成 pytest 与 parity。默认 `init.sh` 不得再增加这些慢门；可用 `./init.sh --standard` 或旧 `SUBLIFT_INIT_*` 环境变量显式转发以兼容历史用法。

## 代码 / 架构约束

- Formatting：Python 由 Ruff 管理，严格 mypy；C++ / Swift 遵循各自现有 target 与目录边界。
- Naming：产品模块保持 `extractor / detector / ocr / pipeline / export` 的 Protocol 分层；Native target 依赖方向以 `docs/cpp/` 契约为准。
- Error handling：产品默认 C++ runtime 必须 fail-closed；Python 仅在显式 `--runtime python` / `SUBLIFT_RUNTIME=python` 下作为 Oracle 或回滚。

  ```python
  if runtime == "python":
      return run_python_oracle(...)
  return run_cpp_worker_or_raise(...)
  ```

- 活样本：`src/sublift/benchmark/config.py`（显式错误、路径解析和严格类型的 Python 约定）；`cpp/src/application/`（Native composition root）。

**Git**：

- 分支：遵循仓库现有分支策略，不擅自切换或重写历史。
- Commit message：Conventional Commits；需要提交时使用 `.agent/skills/commit/`。
- **提交 / 推送必须先获得用户明确同意**；本项目不采用 Harness 的自动提交默认值。

## Always / Ask First / Never

- **Always**
  - 修改已登记的一个当前 Feature；历史 Phase 记录只做明确的兼容迁移。
  - 先运行 `./init.sh`，再按改动范围运行上述真实验证命令。
  - 将最终验证证据写到对应 Phase feature 的 `evidence`，保持 `progress.md` 为简短导航。
  - 对新 Feature 使用五位 ID 与 `docs/phases/phaseN.schema.json` 的 canonical 分支。
- **Ask first**
  - 新增第三方依赖、数据库/破坏性 schema 变更、CI/CD、发布/签名/公证。
  - 修改本文件或 `.agent/` 下的规则与 skill（本次用户明确要求 Harness 迁移是例外）。
  - commit、push、删除历史 evidence 或既有 adapter。
- **Never**
  - 绕过 `.agent/skills/commit/` 直接提交或推送。
  - 将 `feature-list.json` 重新当作进度真源，或伪造旧 feature 的 Harness state / review / commit receipt。
  - 将 C++ parity harness/golden harness 改称或混同为 Agent Harness。

## Definition of Done

同时满足才算完成：

- [ ] 目标行为已实现
- [ ] 与改动范围匹配的验证已实际运行（见标准命令）
- [ ] 验证证据已写入对应 Phase feature 的 `evidence`（格式见 `.agent/rules/project-continuity.md`）
- [ ] `./init.sh` 可通过

## Harness 导航

到时机就读/执行入口，不凭记忆。本表不是能力全表；plan / test / review / verify 由顶层编排按需激活。

| 时机 | 入口 |
|---|---|
| 开始任何工作前 | `.agent/skills/session-bootstrap/SKILL.md` |
| 结束会话前 | `.agent/skills/session-handoff/SKILL.md` |
| 需要提交或推送时 | `.agent/skills/commit/SKILL.md`（仍须先获用户同意） |
| 进度 / Phase 文件约定 | `.agent/rules/project-continuity.md` |
| 完整开发已登记 Feature | `.agent/skills/develop-feature/SKILL.md` |
| 执行 / 接续 / 完成已登记 Phase | `.agent/skills/develop-phase/SKILL.md` |
| 存在活动 Feature / Phase state | 先恢复对应顶层编排，再改产品代码 |

Codex 委派须使用实际子代理调用，并在 receipt 中诚实写 `platform: codex` 与可追溯的 task / invocation reference；当前模板没有 Codex runtime adapter，不得伪造 OpenCode、Claude Code 或其他平台的 attestation。
