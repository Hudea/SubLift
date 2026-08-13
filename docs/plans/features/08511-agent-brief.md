# 执行 Agent Brief：08511 Task Center 信息层级与导出可见性

把下面「Prompt」整段作为用户消息交给执行 agent。本文件是交接副本，不是实施合同；合同仍以 `docs/design_ui/task-center-v2.md` 为准。

---

## Prompt（从下一行复制到文末）

你是 SubLift 仓库里的实现 Agent。只做一件事：在 `feat/task-list` 上按已冻结合同实现 **Feature 08511**，用 **commit 切片**交付（不要开 PR、不要 push），走完 **计划 → TDD 开发 → 审查修复循环 → 验收报告**。

用户已明确授权：08511 相关改动可以按切片 `git commit`。**禁止 push。** 提交必须走 `.agent/skills/commit/SKILL.md` + `.agent/skills/commit-message/SKILL.md`。

你没有 Computer Use，不能点真实菜单/面板。UI 验收用仓库既有 **DEBUG EvidenceShot 应用截图**（与 08308/08309 相同），不要假装做过点击或 VoiceOver。

你的 `spawn_subagent` **只有两种** `subagent_type`：

- `explore`：只读探代码
- `general-purpose`：多步实现、测试、只读审查、写文档

没有 tester / reviewer / docs 专用类型。需要审查时派 `general-purpose`，`capability_mode=read-only`，description 前缀 `[reviewer]`。需要探路时派 `explore`。

---

### 0. 开工（先做这个，再改代码）

1. `pwd` 必须是仓库根。读 `AGENTS.md`。跑 `./init.sh`。失败则停，先修初始化。
2. 确认分支是 `feat/task-list`，工作区干净。不要切分支、不要 rebase `main`、不要碰 `feat/llm-subtitle-correction`。
3. **只读**以下合同（冲突时以 v2 为准，父合同管调度/写入/恢复）：
   - `docs/design_ui/task-center-v2.md`（Accepted；Q1=Yes sidecar 默认；Q2=Yes 公共输出文件夹）
   - `docs/plans/features/08511.md`
   - `docs/design_ui/batch-task-center.md`（不要重写调度）
4. 用 `explore` 子 agent 盘点现状，至少核对：
   - `TaskCenterView` 空态 ZStack + 双 `.fileImporter`
   - `BatchTask` 无 `importRootURL`
   - `BatchQueueModel.importInputs` / `prepareOutputPlan(sourceRoot: nil)`
   - `BatchOutputPlanner` `replacingOccurrences`
   - `BatchTask.requeue()` 已清失败字段（08410-fix，只锁回归）
   - `EvidenceShot.taskCenterFixtureIfRequested` / `makeFixtureState` / `SUBLIFT_EVIDENCE_TASKCENTER`
5. 写一份 **本会话实施清单**（可放在你的 todo 里，不要新造 Phase 编排系统）：C1–C6 六刀，对应 08511.md 的 PR1–PR6，但名称叫 **Commit 切片**，不是 PR。
6. 登记 08511（允许改跟踪文件，这是实施轮）：
   - `docs/phases/phase8.json` 增加 Feature `08511`（canonical 字段、依赖 08410、subtask 按 C1–C6、status `in-progress`）
   - `phases.json` 的 phase8 保持存在；若描述仍写「08001–08410 已完成」则补一句「08511 为后续 UX」
   - `progress.md` 导航改为当前任务 08511
   - **不要**占用 09101，不要发明 Phase 11

然后才能改 Swift。

---

### 1. 硬约束

**允许改：**

- `apps/macos/Sources/SubLiftMac/Core/Batch/*`
- `apps/macos/Sources/SubLiftMac/UI/TaskCenter/*`
- `apps/macos/Tests/SubLiftMacTests/Batch*.swift`
- `apps/macos/Tests/SubLiftMacTests/TaskCenter*.swift`
- `apps/macos/Sources/SubLiftMac/App/EvidenceShot.swift`（仅 Task Center fixture 段）
- `docs/design_ui/task-center-v2.md`、`docs/design_ui/evidence/08511/`、`docs/plans/features/08511.md`
- 实施轮允许：`docs/phases/phase8.json`、`phases.json`、`progress.md`、`docs/design/macos-gui.md` 里与 F24/Task Center 相关的一句同步

**禁止改：**

- `WorkspaceRootView.swift`、`WorkspaceModel.swift`、C++ / Python / UDS / Worker
- 校正分支会撞的非必要文件
- 并行 OCR、目录监听、递归 UI 开关、Table outline、逐任务输出路径、Whisper、ASS/VTT、Sandbox、分发
- bump `schemaVersion`（只加 `decodeIfPresent`）
- 导入期写 SRT、启动 Worker、把任务标成 `skipped`
- 把 Computer Use / VoiceOver / 「已点击 Toolbar」写成已完成

**产品已拍板（不要再问）：**

- Q1：默认 sidecar（SRT 在每个视频旁边）
- Q2：08511 **必须**做队列级「输出位置」菜单（sidecar | 选公共文件夹）
- 文件导入和文件夹导入今天算法相同；非递归时 sidecar 正好在所选文件夹。08511 改的是可见性 + 可选公共根，不是改默认算法

**测试：**

```bash
./init.sh
cd apps/macos && env PYTHONPATH= swift test --filter '<本刀相关 class>'
cd apps/macos && env PYTHONPATH= swift test    # 每刀结束、最终验收都要全绿
git diff --check
```

完整 `swift test` 前必须 `export PYTHONPATH=`（会话 PYTHONPATH 会污染 .venv 子进程）。不要跑完整 `verify-standard.sh`，除非你误改了 C++/Python。

---

### 2. 强制流程（每一刀都走完，禁止跳审查）

对 **C1–C5** 每一刀：

```
A. 计划（5–10 分钟，写在 todo / 切片笔记）
   - 本刀合同要点、允许文件、RED 测试名单、停止条件
B. TDD
   - 先写失败测试并亲眼看到 RED（编译失败或断言失败）
   - 再写最少产品代码到 GREEN
   - 禁止先实现再补测试，禁止用源码字符串断言代替行为
C. 定向验证
   - 本刀 filter 测试 + 本刀会碰到的回归（尤其 requeue 清失败字段）
D. 审查修复循环（至少 1 轮，P0/P1 必须清零）
   - 派 1 个 read-only `general-purpose` `[reviewer]`
   - prompt 里写明：对照 task-center-v2.md 本刀章节；读源码不要只读 diff；
     输出结构化问题（bug / suggestion / nit + file:line）
   - 你修复所有 bug 与会破坏合同的 suggestion
   - 修完再派 **同一个方向** 复核，或新开一个 reviewer 只核「上轮问题是否真修了」
   - 禁止自审冒充独立审查
E. Commit
   - 走 commit skill；一次一刀；message 用 Conventional Commits + 简体中文
   - 建议标题见第 3 节
F. 更新 08511 subtask / progress 导航（短）
```

**C6**（证据 + 文档收口）走第 5 节截图流程，再做 **三路并行只读审查**（三个 `general-purpose` reviewer 同时派）：

1. Domain / 规划 / `sourceRootForPlanning` / skip 语义
2. 持久化 / `outputDestination` save-load / `BatchPath`
3. UI / Inspector / EvidenceShot 诚实度（对照 T01–T07 PNG 是否真的展示合同内容）

修完 P0/P1 再写验收报告。

切片之间不要合并实现。C3 可与 C1 并行思考，但落地时仍建议 C1 → C2 → C3 → C4 → C5 → C6，避免 `TaskCenterView` / `BatchQueueModel` 互相踩。

---

### 3. 六刀合同（名称是 Commit，不是 PR）

细节、类型签名、测试名单以 `docs/plans/features/08511.md` 和 `docs/design_ui/task-center-v2.md` 为准。这里只冻结「每刀必须交付什么」。

#### C1 — Domain：`importRootURL` + 扫描接受项 + `importInputs`

建议 message：`feat(batch): 持久化 importRoot 并让导入写入扫描根`

必须：

- `BatchAcceptedItem { url, importRootURL }`
- 目录扫描 → `importRootURL = 该目录`；直接文件 → `nil`（不要把 parent 提升为 root）
- `BatchTask.importRootURL` 加法 Codable：`decodeIfPresent`，旧 JSON 缺键 → nil
- **必须改** `BatchQueueModel.importInputs`：`BatchTask.make(..., importRootURL: item.importRootURL)`
- 本刀 **不** 规划输出、不改 UI
- RED：Scanner 根归属；Model 导入后 folder 任务 root 非 nil、loose 文件为 nil；缺键 decode；`requeue` 回归仍绿

#### C2 — Output：导入即预览 + `BatchPath` + destination 基础设施

建议 message：`feat(batch): 导入即预览 sidecar 并修复公共根相对路径`

必须：

- `previewTarget`：纯路径 + 公共根逃逸；不查可写、不读冲突、不写文件
- `BatchPath.relativePath`：`hasPrefix(root + "/")` + `dropFirst` + trim 前导 `/`
  - 反例：`/data/v` + `/data/v/show/data/v` → `show/data/v`
  - `/data/video` 不是 `/data/video2` 的前缀
  - **不要**复用 Scanner 现有弱 `hasPrefix(rootPath)`
- 导入后 `planOutputsForWaitingTasks()` 给 waiting 写 `outputURL`；磁盘无新 SRT；无 Worker
- 导入与开始共用 `sourceRootForPlanning(task) == task.importRootURL`
- **禁止** `prepareOutputPlan` 再写死 `sourceRoot: nil`
- 已存在目标：仍 waiting，**禁止**导入期 `skipped`；开始时再确认替换
- `cancelStart` 只清 `pendingConflicts`，**保留**预览 `outputURL`（改现有测试）
- `setOutputURL` 放宽为 `waiting || canRetry`，只改 `outputURL`
- 改目的地或 retry 前：waiting + retryable 都重算预览
- Model 瞬时 `planningErrors: [UUID: String]`，不进 JSON / 不进 `failureMessage`
- `BatchOutputDestination` 显式 JSON `{type:sidecar}` / `{type:publicRoot,url}`
- Repository `save` **必传** `state.outputDestination`；`load` 映射 `?? .sidecar`
- `Scheduler.setOutputDestination` → persist；Model 不得另持一份
- 开始遇 `unwritableDirectory` / `targetOutsideRoot` / 同批碰撞：不启动、alert、保持 waiting
- 同批碰撞：先到先得；后者只有 `planningError`，`outputURL == nil`
- RED 见 08511.md 测试表

本刀可以没有 Toolbar 菜单（菜单在 C5），但 persist 基础设施必须落地。

#### C3 — 空态 + 单 fileImporter

建议 message：`fix(task-center): 空画布 drop zone 与单一 fileImporter`

必须：

- `summary.total == 0`：**不渲染** `TaskTableView`（禁止 ZStack 叠斑马纹）
- 虚线 drop zone + 标题「将视频拖到这里」+ 副标题「拖入视频或文件夹，或使用工具栏添加」+ 隐私句
- **删除**空态蓝色「添加文件」和空态「添加文件夹」
- 筛选栏在空队列时 disabled；筛选无匹配 ≠ 空态
- 单一 `.fileImporter` + `TaskCenterImportKind { files, folder }`
- `beginImport`：先设 kind，再 `DispatchQueue.main.async { isImporting = true }`
- `isDropTargeted` 高亮虚线（Accent + 0.12 fill）
- 增加 DEBUG fixture：`SUBLIFT_EVIDENCE_TASKCENTER_DROP=1`（或等价）强制空态 drop 高亮，供 T02。不要依赖 Computer Use 去拖文件
- 你没有 Computer Use：Toolbar 两个入口用代码审查 + `TaskCenterImportKind` 单测证明；**证据里如实写「未实地点击 NSOpenPanel」**

#### C4 — 位置列 + 搜索 + 四档列宽

建议 message：`feat(task-center): 显示导入位置列并支持路径搜索`

必须：

- 「位置」列在文件与状态之间
- 显示规则按合同 3.3；相对路径对 **源目录** 算：`BatchPath.relativePath(from: importRoot, to: source.deletingLastPathComponent())`
- 源不在 import root 下 → 回退 parent 规则；比较用 `standardizedFileURL`
- Tooltip / AX：`从「Zootopia」导入 · <dir>` 或 `所在文件夹 · <dir>`
- 搜索匹配文件名 + 位置 + 完整 path
- `visibleColumns(forWidth:)`：`<960` 文件/状态/进度；`≥960` +输出；`≥1100` +位置；`≥1280` +引擎/时长/添加时间
- GeometryReader + 多 Table 变体，**不要** `TableColumn.hidden`
- 位置不可见时折进文件单元格
- **不做** outline / disclosure group

#### C5 — Inspector 卡片 + 输出位置菜单

建议 message：`feat(task-center): 原生 Inspector 与输出位置控件`

必须：

- `TaskInspectorModel` 一张卡片；上限约 240pt + 内滚
- 结构见 08511.md「Inspector」；产品 UI **无 Task ID**
- Picker `labelsHidden`，禁止「引擎 引擎」
- 删除 / 上移 / 下移并入卡片页眉，去掉孤立底条
- `inspectorModel(for:fileExists:planningError:)`；`outputExistsWarning` 在目标已存在时 **必显**
- `planningError` 只来自 Model 字典，禁止塞进 `failureMessage`
- Toolbar「输出位置」：默认「视频旁边」；另选公共文件夹
- 输出目录用独立 `NSOpenPanel`（`canChooseFiles=false`, `canChooseDirectories=true`），**不要**第三个 `.fileImporter`，**不要**抄 `WorkspaceRootView.openFile()`（那是选文件）
- 切换 sidecar ↔ 公共根必须重规划 waiting + retryable
- 无逐任务自定义路径

#### C6 — 证据 T01–T07 + 文档收口

建议 message：`docs(task-center): 写入 08511 截图证据并同步合同状态`

必须：

- `docs/design_ui/evidence/08511/`：T01–T07 PNG + `README.md` + 索引
- **必须**改 `EvidenceShot.makeFixtureState("WAITING")`：同一 `importRootURL` + sidecar `outputURL`，否则 T03/T05 是假的
- 需要时扩展 fixture：`SCAN=1`（已有）、DROP 高亮、COMPACT=960、单选 Inspector、公共根菜单态（T07）
- 不冒充真实 OCR；含条目/队列状态用 DEBUG fixture
- 把 08511 的 `evidence` 写入 `docs/phases/phase8.json`（最终验证命令、截图路径、未执行项如实记录）
- 同步 `progress.md`；合同/08511.md 状态改为已实现
- 三路审查 + 完整 `swift test`

---

### 4. TDD 细则

- Scanner / Planner / 导入规划用 **真实临时目录**，不要纯字符串路径假绿
- 禁止 `XCTAssertTrue(source.contains("importRootURL"))` 这类源码扫描
- RED 必须先失败：新类型不存在、或断言失败。把失败原因记进该刀笔记
- `requeue()` 清 `failureMessage` / `progress` / `result` 已在 08410-fix：保持 `BatchTaskTests` / `testDetailAfterRequeueHidesOldFailure` 绿（断言可迁到新 Inspector 模型）
- `testCancelStartDoesNotTouchOutputs` 语义已变：取消开始 **保留** 预览 URL，要改测试而不是改回清 URL

---

### 5. UI 证据：EvidenceShot 应用截图（你没有 Computer Use）

与 08308/08309 相同：DEBUG 构建里 `EvidenceShot` 把 Task Center 窗口渲成 PNG。不依赖系统录屏权限（优先 cacheDisplay，全黑则 PDF，再退 CGWindowList）。

现成入口：

- `SUBLIFT_EVIDENCE_TASKCENTER=EMPTY|WAITING|RUNNING|PAUSED|MIXED|RESTORED|ERROR`
- `SUBLIFT_EVIDENCE_SHOT=<绝对路径.png>`
- `SUBLIFT_EVIDENCE_DELAY`（Task Center 默认约 6 秒）
- `SUBLIFT_EVIDENCE_SCAN=1`：真实临时目录导入，打出扫描摘要横幅
- `SUBLIFT_EVIDENCE_COMPACT=1`：960×600
- `SUBLIFT_EVIDENCE_DARK=1`：窗口 Dark
- `SUBLIFT_EVIDENCE_DROP=1` 是 **Welcome** 的，不是 Task Center。T02 必须自己加 Task Center drop fixture

模板（在 `apps/macos` 下，DEBUG）：

```bash
export PYTHONPATH=
mkdir -p ../../docs/design_ui/evidence/08511

# T01 空态 1280
SUBLIFT_EVIDENCE_TASKCENTER=EMPTY \
SUBLIFT_EVIDENCE_SHOT="$PWD/../../docs/design_ui/evidence/08511/T01-empty-1280.png" \
SUBLIFT_EVIDENCE_DELAY=6 \
swift run --configuration debug SubLiftMac

# 看到 [EvidenceShot] saved ... 后结束进程，再拍下一张
```

建议映射：

| ID | 环境 | 必须看见 | 必须看不见 |
|---|---|---|---|
| T01 | `TASKCENTER=EMPTY` | 虚线 zone、标题、副标题、隐私句 | Table 斑马纹、蓝色「添加文件」 |
| T02 | `EMPTY` + 你新增的 DROP fixture | Accent 虚线 + 浅 fill | 空态按钮 |
| T03 | `WAITING`（改过的 fixture）+ `SCAN=1` | 位置列同文件夹名 + 扫描摘要 | 只有文件名 |
| T04 | `WAITING` + 单选 fixture（可新增 `SELECT=first`） | Inspector 卡片、无 Task ID、单行提取 | 「引擎 引擎」、孤立底条 |
| T05 | `WAITING` 且 fixture 带 sidecar `outputURL` | 输出列是 `name.srt`，文案「字幕将保存到」或「写在每个视频旁边」 | 「—」、completed、真字幕正文 |
| T06 | `WAITING` + `COMPACT=1` | 文件/状态/进度仍在 | 位置列撑破布局 |
| T07 | 公共根 fixture 或 sidecar 菜单可见态 | 「输出位置」默认 sidecar；若注入公共根则路径更新 | 开始前写出 SRT |

拍完后 **你自己用 read_file 打开每张 PNG**，按上表核对。裁掉 Toolbar / 位置列 / 输出列的废片要重拍。不要把 08308 旧图改名充数。

`EvidenceShot.swift` 的 `save()` 只改 Task Center fixture 段和 `makeFixtureState`。不要重写主窗口 `save()` 去跟校正分支抢文件。

---

### 6. 审查 prompt 模板（派 general-purpose、read-only）

```
你是只读审查员。不要改文件。

对照：
- docs/design_ui/task-center-v2.md 第 <本刀章节>
- docs/plans/features/08511.md
本刀范围：<C# 标题与允许文件>
本刀 diff：git diff <slice-base> 或工作区未提交改动

先读源码再下结论。输出：

## Summary
## Issues
### Issue N -- Severity: bug|suggestion|nit
- File: path:LINE
- Description:
- Suggestion:
- Status: open

禁止发明 Computer Use / VoiceOver 已完成。
禁止把「测试绿」当成合同已满足——要看断言是否打到不变量。
```

C6 三路审查把「本刀」换成全 08511，并分别指定 Domain / Persist / UI+PNG。

P0/P1（bug、合同违背）必须修完再 commit。nit 可记入 evidence「已知偏差」。

---

### 7. 验收报告（C6 结束后写给用户，同时写入 08511.evidence）

必须包含：

1. 六刀 commit hash + message
2. 合同 12 条验收逐条：通过 / 未做 / 部分（部分必须说明）
3. 最终命令与退出码：`./init.sh`、完整 `swift test`（写出 XCTest + Swift Testing 数量）、`git diff --check`
4. T01–T07 路径；你读图后的一句话核验；废片/重拍记录
5. **未执行项（必须诚实）**：无 Computer Use，故无真实点击 Toolbar / 无 VoiceOver / 无把文件拖进窗口。双 importer 只靠协调器代码 + 单测
6. 独立审查轮次与剩余未修 nit
7. 风险：与 `feat/llm-subtitle-correction` 的文件碰撞（应仍只在 Batch*/TaskCenter/EvidenceShot fixture）

08511 全部完成后才能把 phase8.json 里该 Feature 标 `done`。不要在 C3 就标完。

---

### 8. 停止并汇报（遇到这些立刻停）

- 需要改 Worker / UDS / 默认 runtime / Workspace 组合根
- 需要 bump schemaVersion 才能 load 旧清单
- 完整 `swift test` 红且不是你引入的，先确认是不是 PYTHONPATH / 未编译 worker
- 用户未再授权却要 push 或开 PR
- 想做 outline、递归开关、逐任务输出路径

停的时候写清阻塞和已完成切片，不要半套实现标 done。

---

先跑第 0 节开工，然后从 C1 的 RED 测试开始。不要先改 UI。
