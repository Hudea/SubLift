# AGENTS.md

> SubLift 的项目级 Agent 契约：只放稳定事实、可复制命令与长期边界。流程细节在 `.agent/` 中按需读取，不整体导入；当前 Phase 只由 `phases.json` 与 `progress.md` 声明。

## 项目概述

- **项目名**：`SubLift`
- **目标**：本地提取视频中烧录的硬字幕，输出可编辑的 SRT（ASS/VTT 后续扩展）。
- **技术栈**：C++20 / ObjC++（CMake + Ninja）、Swift 5.9+（SwiftPM）、Vue 3 / TypeScript、ffmpeg；macOS 13+ 的 Vision 可选，Paddle 使用 Native ONNX Runtime / OpenCV。Python 3.12+（`uv`）仅服务隔离的 benchmark、诊断与历史 Oracle 迁移工具。
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

- `src/sublift/`：冻结 Oracle 与离线工具内部实现（benchmark/诊断）；不是产品运行时，不随 Native 新功能演进。
- `src/sublift_offline/`：可选离线工具的公开命名空间（`python -m sublift_offline`）。
- `cpp/`：Native C++ Core、Worker、Vision/Paddle adapters 与 CTest。
- `apps/macos/`：SwiftUI 开发者 GUI 与 Swift 测试。
- `apps/web/`：Vue 3 / TypeScript Web UI 与 Vitest。
- `benchmark/`：版本化 configs、datasets、baselines 与 parity 资产；本机运行产物写 `debug/benchmark/`。
- `.agent/`：轻量项目规则、会话入口与提交辅助；已归档的复杂能力编排不属于当前活跃 Harness。

## 环境与标准命令

**运行 / 构建**：

- 会话初始化：`./init.sh`
- Native Debug 构建：`cmake -S cpp -B build/cpp -G Ninja -DCMAKE_BUILD_TYPE=Debug -DSUBLIFT_REQUIRE_OPENCV=ON -DSUBLIFT_ENABLE_VISION=ON && cmake --build build/cpp`
- macOS GUI：`cd apps/macos && swift build && swift run SubLiftMac`
- 隔离离线工具：`./scripts/verify-offline.sh`（benchmark / 评分 / 冻结 Oracle；不是产品门）

**测试与验证**：

- C++：`ctest --test-dir build/cpp --output-on-failure`
- Swift（macOS）：`cd apps/macos && swift test`
- Web：`npm --prefix apps/web test`
- 隔离 Python 工具（仅修改对应树时）：`uv sync --extra oracle --extra vision --extra paddle`，然后 `uv run ruff check .`、`uv run mypy src tests`、`uv run pytest -m "not integration" --no-cov`

`./init.sh` 只建立开工前提；依赖同步、构建与测试由独立命令承担。产品行为以 `./scripts/verify-product.sh` 为准。可选 Python 工具走 `./scripts/verify-offline.sh`；`scripts/verify-standard.sh` 仍是含离线 Oracle 的过渡全仓门。

## 代码 / 架构约束

- C++ / Swift / TypeScript 遵循现有 target 与目录边界；维护隔离 Python 工具时仍遵循 `pyproject.toml` 的 Ruff 与 strict mypy 配置。
- 产品模块保持 `extractor / detector / ocr / pipeline / export` 的 Protocol 分层；Native target 依赖方向以 `docs/cpp/` 契约为准。
- Phase 13 产品只有 C++ runtime；Worker、engine、模型或 capability 缺失必须 fail-closed，不得切换到 Python 或其他引擎。产品回滚通过回滚到上一已验收版本完成，不是同版本 runtime 切换。

  ```text
  resolve native worker + requested engine
    → available: run C++
    → unavailable: fail closed
  ```

- Python CLI/IPC 路由、`.venv` 驱动的 Worker 查找、RapidOCR 模型缓存和从 Python wheel 取 ORT 已从产品路径清除；不得重新引入。
- 活样本：`cpp/src/application/`（Native composition root）；`src/sublift/benchmark/config.py` 仅作为隔离工具的严格配置样本。

## Phase / Deliverable / Task

- **Phase**：一个完整、可验收的产品能力；不能用单个修复、审计轮次或技术步骤充当 Phase。
- **Deliverable**：组成 Phase 的纵向结果，每个新 Phase 包含 2–8 个可独立验收的 Deliverable。
- **Task**：实现 Deliverable 的会话内执行步骤；不分配长期 ID，不写入 Phase JSON，不沉积为历史状态链。
- **Phase 状态**：`not-started → in-progress → ready-for-merge → done`；`blocked` 仅表达真实阻塞，不代替执行状态。
- **Deliverable 状态**：`not-started → in-progress → done`，遇到真实阻塞时可标记 `blocked`；不使用 `ready-for-merge`。
- `ready-for-merge` 表示实现与分支验收完成、仍可修改；只有合入 `main` 且在 `main` 上完成必要验收后才能置 `done`。
- `done` Phase 只读；仅允许事实勘误。后续缺陷按同一能力或发布目标聚合为新的维护 Phase
  及其 2–8 个 Deliverable；单个零散修复不得独立升格为 Phase，也不得继续扩张旧 Phase。
- Phase 1–12 保留 legacy 结构与 evidence，不为套用新模板而重写历史。

**Git**：

- 遵循仓库现有分支策略，不擅自切换或重写历史。
- Commit message 使用 Conventional Commits，并经 `.agent/skills/commit/SKILL.md` 起草与执行。
- **提交 / 推送必须先获得用户明确同意。**

## Always / Ask First / Never

- **Always**
  - 一次只推进一个已登记的当前 Deliverable；历史 Phase 只做明确的兼容迁移或事实勘误。
  - 开工与交接时运行 `./init.sh`，交付前运行与改动范围匹配的真实验证。
  - 将最小、可复核的最终验证证据写入对应 Deliverable 的 `evidence`，保持 `progress.md` 为简短导航。
  - 新 Phase 使用 2–8 个纵向 Deliverable，并通过当前 `docs/phases/phaseN.schema.json`；Task 只放在会话计划中。
- **Ask first**
  - 新增第三方依赖、数据库或破坏性 schema 变更、CI/CD、发布、签名、公证。
  - 修改本文件或 `.agent/` 下的规则与 skill。
  - commit、push、删除历史 evidence、既有 adapter、本地测试素材或分支。
- **Never**
  - 绕过 `.agent/skills/commit/` 直接提交或推送。
  - 将 `feature-list.json` 重新当作进度真源，或伪造已归档能力包的 state / review / receipt。
  - 将 C++ parity/golden harness 与 Agent Harness 混为同一概念。
  - 向已 `done` 的 Phase 追加功能、返工批次或新的执行 Task。

## Definition of Done

Deliverable 同时满足才可标记为 `done`：

- [ ] 目标行为或文档目标已实现
- [ ] 与改动范围匹配的验证已实际运行
- [ ] 最终验证证据已写入对应 Deliverable 的 `evidence`
- [ ] `./init.sh` 可通过

全部 Deliverable 完成且分支验收通过后，Phase 才可进入 `ready-for-merge`。变更合入 `main`
并在 `main` 上完成必要验收后，Phase 才可置 `done` 并转为只读。

## Harness 导航

| 时机 | 入口 |
|---|---|
| 开始任何工作前 | `.agent/skills/session-bootstrap/SKILL.md` |
| 结束会话前 | `.agent/skills/session-handoff/SKILL.md` |
| 需要提交或推送时 | `.agent/skills/commit/SKILL.md`（仍须先获用户同意） |
| 进度 / Phase 文件约定 | `.agent/rules/project-continuity.md` |
| 初始化职责与扩展边界 | `.agent/rules/initialization.md` |

Planning、Testing、Review、Verification 与 Task 编排不持久化为 Harness 状态链；未来仅在项目真实需要时增量引入，不以历史文件或聊天记忆模拟进度事实。
