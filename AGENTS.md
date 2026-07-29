# AGENTS.md

SubLift 是一个硬字幕（烧录字幕）提取工具，从视频画面中自动识别字幕，生成可编辑的 SRT / ASS / VTT 文件。初步目标是实现从 cli 到 macos 平台，将使用 Python+Swift 来完成，最终目标是实现跨平台。

## 启动流程

在编写代码之前：

1. **确认当前工作目录**：运行 `pwd`（应为 SubLift 项目根目录）
2. **完整阅读本文档**
3. **阅读项目文档**：
   - `docs/ARCHITECTURE.md` — 架构设计（模块职责、数据流、技术栈；feat-004 填充前见 `docs/plans/`）
   - `docs/REQUIREMENTS.md` — 项目依赖和项目规格
   - `README.md` — 快速上手与 CLI 用法（feat-004 填充前为占位）
4. **运行 `./init.sh`**，确认环境状态正常
5. **阅读 `feature-list.json`**，了解项目级功能总览以及当前的开发状态；如需查看当前 Phase 的开发细节，打开对应的 `docs/phases/phaseN.json`；如需查看设计源头，打开 `docs/plans/`
6. **查看最近提交**：运行 `git log --oneline -5`

如果基线验证失败，请先修复它，再添加新的工作范围。

## 项目技术栈

- **语言**：Python 3.12+
- **包管理**：uv
- **OCR 引擎**：Apple Vision（macOS 可选依赖）、PaddleOCR（通用可选依赖）

## 关键架构约束

- 采用模块化的架构设计，功能都应该可替换，可插拔，功能模块低耦合高内聚，设计恰当的接口。

## 工作规则

- **一次只做一个功能**：从当前 Phase 的 `docs/phases/phaseN.json` 中只选择一个未完成的功能
- **任务粒度**：一个任务 = 一个可独立验收的功能块；任务内部细节通过 `subtasks` 字段承载，不拆成独立任务。粒度以「能否独立验收」为准
- **必须验证**：未运行验证命令前，不要声称任务已完成
- **更新产物**：结束会话前，更新 `progress.md` 和对应的 Phase 细节文件（`docs/phases/phaseN.json`）；如大功能块状态变化，同步更新 `feature-list.json`
- **保持范围聚焦**：不要修改与当前功能无关的文件
- **保持干净状态**：下一次会话必须能够立即运行 `./init.sh`
- **提交规则**：未经用户同意，严禁提交工作区改动

## 必需产物

- `feature-list.json` — 项目级功能总览（按 Phase 分组的大功能块）
- `docs/phases/phaseN.json` — Phase 级功能跟踪（开发细节与验证证据的唯一事实来源）
- `docs/plans/` — Phase 设计源头（模块布局、数据流、任务拆分与执行顺序）
- `progress.md` — 会话连续性**导航**日志（保持精简，见下方防膨胀规则）
- `docs/DECISIONS.md` — 架构决策归档（长期累积，按时间倒序）
- `docs/HURDLES.md` — 阻塞开发的任务总结，解决流程，失败原因，最终解决方案和原理。
- `init.sh` — 标准启动与验证路径
- `session-handoff.md` — 可选，适用于较大的会话

## 完成定义

只有同时满足以下所有条件，功能才算完成：

- [ ] 目标行为已经实现
- [ ] 必需的验证已经实际运行（测试 / lint / 类型检查）
- [ ] 验证证据已记录在 `docs/phases/phaseN.json`（`progress.md` 不作为证据主存储）
- [ ] 仓库仍可通过标准启动路径重新启动

## 会话结束

结束会话前：

1. 在 `progress.md` 中更新当前状态，并**清理**本次会话的临时内容（见防膨胀规则）
2. 在对应的 `docs/phases/phaseN.json` 中更新功能状态；如大功能块状态变化，同步更新 `feature-list.json`
3. 记录所有尚未解决的风险或阻塞项
4. 保持仓库足够干净，使下一次会话能够立即运行 `./init.sh`

## `progress.md` 防膨胀规则

`progress.md` 是**导航文件**，不是历史档案。目标：下次打开项目 5 秒内知道在哪里。

| 节 | 保留策略 |
|---|---|
| 当前状态 | 始终保持，每次更新 |
| 进行中 | 仅当前任务，完成即移出 |
| 近期完成 | 最多保留 5 条，超出的压缩为「见 phase2.json」 |
| 阻塞项/风险 | 长期保留，但已解决的立即删除 |
| 近期决策 | 最多 3 条；超出的迁移到 `docs/DECISIONS.md` |
| 本次会话修改的文件 | **会话结束时删除该节** |
| 完成证据 | **不写入 progress.md**，只写入 `phaseN.json` |

## 验证命令

日常启动门（`./init.sh` 默认）：

```bash
uv sync --extra vision --extra paddle
uv run ruff check .
uv run mypy src tests
# C++: cmake + build + ctest（build/cpp Debug；macOS 默认 VISION=ON）
uv run pytest -m "not integration" --no-cov   # 单次；须在 C++ 构建之后以便 IPC e2e 不 skip
uv run python scripts/parity/check_cutover_gate.py --check --skip-runtime --skip-gt \
  --report-out /tmp/sublift_cutover_gate.md
```

必需检查：

- `uv run ruff check .` — lint，必须 0 error
- `uv run mypy src tests` — 类型检查（strict），必须 no issues
- `uv run pytest -m "not integration"` — 单元/IPC 测试全绿（init 默认加 `--no-cov`）
- C++ `ctest` + cutover **parity goldens**（init 默认）

发布 / 完整 cutover 门（**不在**默认 init 内，需显式）：

```bash
SUBLIFT_INIT_RUNTIME=1 SUBLIFT_INIT_REQUIRE_GT=1 ./init.sh
# 或
uv run python scripts/parity/check_cutover_gate.py --check --require-gt
```

集成测试（需外部资源）：

```bash
uv run pytest -m integration
```

## `init.sh` 范围与防膨胀

`init.sh` 是**日常可重复启动门**，不是完整发布 CI 的无限堆积处。目标：干净仓库上数分钟内可绿，且不写入应入库的报告垃圾。

| 允许进入默认 init | 禁止默认塞入（须 env / 独立脚本 / 发布 job） |
|---|---|
| 工具链存在性、`uv sync` 产品 extras | 无文档的「顺手再跑一遍」重复门 |
| ruff / mypy | 默认 coverage 报告（用 pytest 配置显式开） |
| **一次** pytest（`not integration`，构建 C++ 后） | 同一测试集拆成多次 pytest 调用 |
| C++ configure/build/**全量 ctest**（Debug 树） | 无理由的第二套 build 目录全量编测 |
| cutover **正确性 parity**（golden 列表） | 默认 runtime 微基准、默认 live GT L3 |
| 报告写 `/tmp` 或 `debug/` | 每次 init 改写 `docs/reports/*` 污染 git |

**新增步骤前必须自问（写入 PR / 会话说明）：**

1. 是否已被 ruff/mypy/pytest/ctest/parity 之一覆盖？是则 **禁止**再加平行门。  
2. 是否每次会话启动都需要？否 → 环境变量默认关，或独立 `scripts/` / CI job。  
3. 是否会把产物写进版本库？默认否；需要归档时显式路径。  
4. 是否改变「下一次会话能立刻 `./init.sh`」的假设？若变慢一个数量级，必须同步改本文与 README。

**反模式：** 为单个 feat 在 init 末尾永久追加「再跑某某慢脚本」；应用 `SUBLIFT_INIT_*=1` 或 phase 专用验证命令代替。

## 升级处理

如果遇到以下情况：
- **架构决策**：先查阅 `docs/ARCHITECTURE.md`；不确定则询问用户
- **需求不明确**：先查阅 `docs/REQUIREMENTS.md`；不确定则询问用户
- **模块设计问题**：查阅 `docs/design/` 下对应模块设计文档
- **测试反复失败**：更新进度，并标记为需要人工审查
- **范围不明确**：先阅读 `feature-list.json` 确定当前 Phase，再阅读 `docs/phases/phaseN.json` 中的完成定义
