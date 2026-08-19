# 架构决策记录

记录项目中已确认的技术决策，按时间倒序排列。  
**`progress.md` 只保留最近 3 条决策，旧条目归档至此。**

---

## ADR-0038 退役 Python Runtime；产品收口为 Native-only（2026-08-19）

- **状态**：已确认；Phase 13 正在迁移，本文只冻结目标与边界，不表示现存 Python
  CLI、Pipeline、IPC 或验证工具已经删除。
- **背景**：Native CLI、C++ Worker、Native Server 及 Vision/Paddle/Mock Adapters 已具备完整
  产品链路；继续让 Python 同时承担产品 CLI、运行时回滚和行为 Oracle，会保留两套可执行
  Pipeline、两套资源发现与两套故障语义。该结构增加分发体积、环境耦合和双实现漂移，也让
  Native capability 缺失时的产品责任变得不清晰。Python 曾为迁移提供安全基线，但不再是
  长期产品运行时。
- **决策**：
  1. C++ Native 是唯一产品运行时。macOS 使用 Swift → UDS → C++ Worker，Native CLI 使用
     UDS → C++ Worker，Web 使用 HTTP/SSE → Native Server → 进程内 Application/Pipeline；
     产品接口不再保留 Python runtime 选择。
  2. Native 引擎、模型、ORT、FFmpeg 或其他 capability 缺失时必须 fail-closed，不得启动
     Python、切换 OCR 引擎或伪装为空结果。产品回滚采用上一版已验收的 Native artifact、
     release/tag 或 Git revision，不采用同一版本内的 Python runtime fallback。
  3. Phase 13 负责移除产品可达的 Python CLI、Python Pipeline、Python UDS Server、
     `runtime=python` / `SUBLIFT_RUNTIME=python` 路由及宿主启动接线。迁移完成前这些代码属于
     明确的过渡债务，不得据此继续扩张产品契约。
  4. Python 最多可作为隔离、可选、离线的研究或数据生成工具存在；它不得被产品 target
     import、启动或探测，不得成为安装、构建、启动、提取、导出或 Native 日常验证的前置条件。
  5. 产品行为真源改为版本化 golden、固定 GT 与 Native tests。历史 golden 中的
     `oracle_commit` 继续保存来源证明，但运行中的 Python 实现不再拥有重新解释既有 golden
     或否决 Native 合同的权力。
  6. 最终 Python 产品源码从工作树移除时，以 Git 历史和专用 tag 保存最后可运行 revision；
     不在仓库中创建 `archive/`、复制源码快照或维护第二份冻结实现。
- **理由**：单一 Native 产品运行时使故障语义、资源交付与版本回滚可验证；把历史基线压缩为
  不可变资产和 Git revision，仍保留追溯能力，同时停止为双运行时支付长期维护成本。
- **影响**：Phase 13 在代码、构建、测试与文档中逐步完成迁移，并以无 Python 环境的产品门
  验收。本 ADR 取代 ADR-0021、ADR-0029、ADR-0030、ADR-0033 中“保留 Python 产品回滚”
  的现行效力；旧 ADR 与 Phase evidence 原样保留，继续表示当时的决策和事实。

---

## ADR-0037 Phase 8 采用独立 Task Center 与可靠串行队列（2026-08-12）

- **状态**：已确认；08001 完成设计/架构基线，产品实现从 08102 开始。
- **背景**：Phase 10 已把单视频 Workspace 的状态所有权、提取安全、编辑和导出收口。F24
  仍一次只能处理一个视频；外部第 8 张参考图提出 Task Center，但同时包含 Whisper、ETA 和
  多任务图景，不能直接当作当前能力。长批量任务还必须解决目录扫描、配置漂移、输出覆盖、
  单项失败、取消 teardown、内存增长和应用重启恢复，而不应继续膨胀 `WorkspaceModel`。
- **决策**：
  1. 新增独立 `BatchQueueModel` 组合根；Workspace 继续拥有单视频预览、交互区域、校对与手动
     导出。两者只共享值类型、无状态策略和 Runner port，不共享可变 `SubtitleExtractor`。
  2. Phase 8 固定单并发。暂停是“完成当前任务后暂停”；取消必须等当前 Worker/socket 完全
     teardown 后才能启动下一项。task ID + run token 拒绝迟到事件。
  3. 文件/文件夹统一进入 `BatchInputScanner`；默认不递归、不跟随 symlink，复用
     `VideoImportPolicy`，扫描阶段不启动 OCR。任务复制入队配置，preparing 后锁定；批量区域
     使用 Worker 默认底部区域。
  4. 输出默认 sidecar SRT，冲突默认跳过，也可稳定重命名或明确确认替换；`SrtFormatter` 的
     结果通过同卷临时文件原子落地，completed 后不持有完整字幕数组。
  5. 队列使用 Application Support 下 versioned Codable JSON 原子保存。重启时活动任务变为
     interrupted、队列 paused，用户显式继续前不拉起 Worker；损坏/未知版本 fail-closed。
  6. 本 Phase 不新增数据库、并行 OCR、目录监听、Automatic/Whisper、新格式、Worker/IPC、
     App Sandbox 或分发范围。
- **理由**：批量编排与单视频编辑是两个生命周期。独立组合根和可注入 ports 让 Scanner、
  Scheduler、Writer、Repository 可确定性 TDD；固定串行则复用已经验收的单任务资源与取消边界，
  不在缺少资源/质量证据时引入并发风险。安全输出和显式恢复避免长任务产生不可逆覆盖或假续跑。
- **影响**：产品与视觉合同在 `docs/design_ui/batch-task-center.md`，架构和 Feature 顺序在
  `docs/plans/architecture/phase8-batch-task-center.md`，操作跟踪在 `docs/phases/phase8.json`。
  08001 只完成规划，F24 在 08102–08410 实施并通过最终门前仍标为未实现。

---

## ADR-0036 合并项目辅助架构记录并释放 Phase 9（2026-08-12）

- **状态**：已确认，由 07002 实施。
- **背景**：Phase 7 初次 Harness 迁移与原 Phase 9 仓库稳定化都服务于项目辅助架构；后者
  包含 Harness 收缩、验证入口、诊断脚本、benchmark 兼容和本地产物卫生，并未形成独立的
  产品能力阶段。继续占用根索引中的 `phase9` 会阻碍项目为下一项真实范围建立新的 Phase 9。
- **决策**：
  1. Phase 7 更名为 `Project Auxiliary Architecture`，吸收原 Phase 9 的 `09001–09006`。
  2. `09001–09006`、subtask、依赖和 evidence 作为稳定历史记录原样保留，不改写为 `070xx`。
  3. 根 `phases.json` 移除原 `phase9`，旧 `docs/phases/phase9.json` 容器删除；其历史记录的
     操作真源改为 `docs/phases/phase7.json`。
  4. `phase9` 索引 ID 与 `docs/phases/phase9.json` 路径立即可复用。未来新 Phase 9 从未占用的
     `09101` 开始登记，避免与原批次的 `09001–09006` 冲突；本决策不预设新 Phase 9 的范围。
  5. 历史计划、ADR、诊断表和 evidence 中的“Phase 9”仍表示 2026-08-10 当时的批次名称；
     现行导航必须标明其已归档到 Phase 7，不能再把旧路径当作当前真源。
- **理由**：Phase 是当前工作的领域容器，Feature ID/evidence 是审计标识。移动容器但保留稳定
  ID，既能校正领域结构并释放 Phase 9，也不会制造历史重编号、断链或伪造证据。
- **影响**：Phase 7 同时保存两次项目辅助架构演进；新 Phase 9 可按正常 canonical 结构创建，
  且不依赖旧 Phase 9 的状态或 detail 文件。

---

## ADR-0035 Phase 10 采用 Native Workbench 与处理期只读 Transcript（2026-08-11）

- **状态**：已确认并由 10001–10415 实施；10416 完成当前文档收口。
- **背景**：现有 SwiftUI GUI 已具备单视频导入、预览、字幕区域、流式结果、编辑、取消和
  SRT 导出，但 `ContentView` 直接组合多组状态，metadata/候选/提取控件长期占据主内容。
  `push_entry` 会先追加到 Editor，final entries 又会整体替换；若处理时允许编辑，会产生
  用户修改被覆盖的风险。外部 vNext 设计同时包含 Task Center、Automatic、Whisper 等未来图景，
  与当前 F10/F24 和引擎矩阵并不等价。
- **决策**：
  1. Phase 10 采用一个 Window 一个视频 Session 的 Native Workbench；主窗口由 Video、
     Transcript 与可选 Context Inspector 组成，不增加永久左 Sidebar。
  2. 新增 WorkspaceModel/State 只负责组合现有 focused Core models 与 command availability；
     不把 IPC、算法、坐标、编辑或导出实现搬进 View。
  3. processing/finalizing 的 Live Transcript 只允许选择、seek、浏览和搜索；final entries
     原子落地后才进入可编辑 Review。
  4. Toolbar/Menu/Context menu 复用同一 commands；Session 参数进 Inspector，跨 Session
     偏好进 Settings，Mock/Python Oracle 进入 Advanced/Developer。
  5. Task Center/批量、Automatic engine、Whisper、ASS/VTT、模型下载和独立分发不因参考图
     进入 Phase 10；只有真实契约、数据和测试存在时才显示入口。
- **理由**：该结构强化当前单视频核心路径，保留 C++/Python runtime 和 UDS 边界，并用明确
  状态所有权消除数据覆盖风险；系统组件也能自然适配 macOS 13+、浅深色和辅助功能。
- **影响**：设计真源为 `docs/design_ui/`，Feature 顺序与证据在 `docs/phases/phase10.json`；
  当前 GUI 已采用 Native Workbench，现行模块与数据流由 `docs/design/macos-gui.md` 描述。

---

## ADR-0034 标准验证入口与 Harness 初始化命名解耦（2026-08-10）

- **状态**：已确认并由 09002 实施。
- **背景**：`./init.sh` 已收缩为会话开工所需的轻量 Harness L0，但完整产品门
  `scripts/verify-standard.sh` 仍沿用 `SUBLIFT_INIT_*` 环境变量、临时日志和“日常 init”措辞。
  这会把验证门误导为初始化职责，也与 ADR-0033 的边界相冲突。
- **决策**：标准验证脚本只接受 `SUBLIFT_VERIFY_SKIP_VISION`、
  `SUBLIFT_VERIFY_RUNTIME`、`SUBLIFT_VERIFY_GT`、`SUBLIFT_VERIFY_REQUIRE_GT` 与
  `SUBLIFT_VERIFY_SKIP_CUTOVER`；临时日志统一为 `/tmp/sublift_verify*` 语义。旧
  `SUBLIFT_INIT_*` 不保留 alias、warning 或转发。`./init.sh` 的内容和职责不变。
- **理由**：接口名称应准确区分“可靠开工前提”与“产品交付验证”。直接切换可以避免延长
  已废止 Harness 命名的兼容债；当前没有稳定的外部自动化接口需要兼容窗。
- **影响**：活跃命令使用 `SUBLIFT_VERIFY_*`；ADR-0032、历史 Phase evidence 和 Harness
  migration 计划保留其当时的 `SUBLIFT_INIT_*` 叙述，作为历史事实而不改写。

---

## ADR-0033 精简活跃 Harness；校正运行时与历史 Phase 边界（2026-08-10）

- **状态**：已确认并由 09001 实施。
- **背景**：Phase 7 初次迁移引入了完整能力编排包和兼容转发；随后 canonical 模板
  `/Users/hudea/Project/my_Harness` 的 `subtraction` 版本将复杂编排归档，只保留轻量规则、
  session/commit skills 与初始化策略。SubLift 工作区已经删除旧能力文件，但 `AGENTS.md`、
  `init.sh` 和项目文档仍按旧布局引用它们；GUI 文档也仍把 Python Worker 写成默认后端。
  另有未合入 main 的 Phase 8 UI 分支实验，以及 Phase 2/3/6 中已明确后置却让根 Phase
  长期显示 blocked 的历史任务。
- **决策**：
  1. SubLift 活跃 `.agent/` 以 subtraction 模板的活跃文件为准；模板 `archive/` 是模板仓库
     历史，不复制回项目，也不模拟已经移出的 state / receipt / gate。
  2. `./init.sh` 只检查 `AGENTS.md`、`progress.md`、`phases.json` 与其 detail JSON 链；
     不再提供 `--standard` 或旧环境变量转发。完整产品门继续由
     `scripts/verify-standard.sh` 独立承担。
  3. 项目文档统一描述当前运行时：GUI/CLI 默认 C++ Worker，Python 仅由显式 runtime
     进入 Oracle / 开发回滚；UDS framing 与业务协议保持共享。
  4. Phase 2、3、6 的既定开发范围视为完成，根索引标为 done；各 detail 文件中的 blocked
     历史任务和 evidence 原样冻结，表示未来分发或新数据范围，不代表当前存在活动阻塞。
  5. 未合入 main 的 `UI/macos-ui-redesign` 保留为 branch-only 实验，不恢复、不合并；其
     Phase 8 编号不复用，main 的文档稳定工作登记为 Phase 9。
  6. Benchmark 正式代码与版本化资产继续保留；固定本地媒体位于 `debug/` 根目录，
     imports/runs/perf/archive 位于 `debug/benchmark/`，历史包装器仍在兼容窗口内。
- **理由**：活跃契约必须与真实文件布局一致；初始化只证明“能否可靠开工”，不能再次膨胀成
  交付门。历史 deferred 工作与当前 blocker 分开后，进度导航才不会持续制造伪进行中状态。
- **影响**：ADR-0032 的 Phase 索引和 legacy/canonical 兼容决策继续有效；其中完整能力编排
  布局、`init.sh --standard` 与旧 flag 转发已由本 ADR 取代。产品行为不变，后续复杂 Harness
  能力只在出现真实需求时增量引入。

---

## ADR-0032 采用新 Harness；以兼容层保留历史追踪与完整产品门（2026-08-03）

- **状态**：历史已实施；活跃 Harness 布局与初始化入口已由 ADR-0033 收缩。
- **背景**：项目原先以 `feature-list.json` 和重型 `init.sh` 作为协作入口；新的
  `/Volumes/lab/pp/my_Harness` 提供 `.agent/` 编排协议、`phases.json → detail_file` 索引和
  秒级 L0。SubLift 已有 111 条 `feat-*` 历史记录、非模板字段/状态、两个小数 Phase 文件名，
  且 benchmark root discovery 和历史文档仍依赖 `feature-list.json`。
- **决策**：
  1. 引入 canonical `.agent/`，将 `phases.json` 设为唯一操作性 Phase 索引；新 Feature 使用
     五位 ID、`module[]`、`acceptance_criteria` 与规范 subtasks。
  2. 保留根 `feature-list.json` 为历史兼容索引，不再用它选择当前任务；benchmark root
     discovery 先接受 `phases.json`，同时保留它的 fallback。
  3. `docs/phases/phaseN.schema.json` 使用不重叠的 legacy/canonical 分支：legacy 仅承接
     既有 `feat-*` evidence、string module、旧 subtasks 和 archived/completed 状态，不能成为
     新任务的逃逸口。
  4. `phase4.1.json` / `phase4.2.json` 更名为 `phase41.json` / `phase42.json`，Phase index
     保留 `phase4.1` / `phase4.2` 显示 ID，避免复制一份可编辑历史正文。
  5. 默认 `./init.sh` 仅做 Harness L0；原依赖同步、ruff、mypy、C++/ctest、pytest 与 parity
     日常门整体迁入 `scripts/verify-standard.sh`。`./init.sh --standard` 和旧
     `SUBLIFT_INIT_*` 环境变量显式转发以减少调用方断裂。
  6. Codex receipt 只能记录实际 `platform: codex` 委派，不伪造 OpenCode/Claude Code adapter
     或为本次 bootstrap 回填历史 Feature state、review/commit receipt。
- **理由**：机械重写全部历史 ID/evidence 会制造不可审计的大规模变更，且会破坏历史链接和
  benchmark marker；双真源又会让后续 Harness 无法可靠恢复。兼容层把一次性历史差异限定在
  schema 中，让新工作从 Phase 7 起遵循严格协议。
- **影响**：后续会话从 `session-bootstrap` 进入，以 `phases.json` 定位工作；完整产品验证须
  显式运行标准脚本；本项目继续要求用户明确同意后才能 commit/push。

---

## ADR-0031 Benchmark 代码入正式包；配置、数据、基线与本机产物分层（2026-07-30）

- **状态**：已确认并实施。
- **背景**：原 `benchmark/` 同时是 Python 包、manifest/GT 容器和版本化报告目录；
  `scripts/` 又散落运行、扫描、扰动和 ROI 对照入口。代码未进入 `src` 安装包，依赖仓库根
  `sys.path`；单 manifest 入口只能运行一组，扩参数时不断增加一次性脚本。GUI/C++ 导出
  评分还需要手写 Python API。
- **决策**：
  1. 可执行代码迁入 `src/sublift/benchmark/`，随 `sublift` 包安装并进入 strict mypy；
     `RunConfig` 归配置层，配置解析不再反向依赖 runner。
  2. 仓库根 `benchmark/` 只保留 `configs / datasets / baselines / parity` 四类入库资产；
     固定本地媒体沿用版本化配置引用的 `debug/` 根路径且不入库，本机 imports、runs、perf
     与历史归档统一放 `debug/benchmark/`。
  3. 唯一新入口为 `sublift-benchmark`，提供 `run / matrix / score / show / overhead /
     compare-roi`；历史三个脚本只保留转发窗口。
  4. v2 config 分离 `run` 与 `matrix`；通用 dotted `--set` 和 `--vary` 扩参数，未知字段
     fail-fast，矩阵默认限制 64 个组合并支持 dry-run；算法参数集中为 `pipeline.*`，
     嵌套 `signature.* / change_point.*` 直接从产品 Config dataclass 派生白名单。
  5. Python `run/matrix` 负责同源 Pipeline 与阶段埋点；任何 runtime 的已有 SRT 都通过
     `score` 复用统一质量口径。C++ 未提供同构 recorder 前，不伪造阶段级性能对比。
- **理由**：目录边界、依赖方向和入口身份必须先稳定，参数扩展才不会继续制造脚本和
  manifest 分支。把外部 SRT 评分与执行解耦，也能在 C++/Python cutover 后保持一个质量
  真源。
- **结果**：支持 Vision/Paddle/Mock 单组与矩阵、已有 SRT 标准四件套、矩阵聚合报告、
  recorder 交错扰动和 ROI 硬门；新增普通 Pipeline 参数无需再写 CLI 开关或扫描脚本；
  旧 `debug/benchmark-reports` / `debug/perf_reports` 原样归档，不删除历史证据。

---

## ADR-0030 Phase 6.9 止于开发架构收口；产品分发整体后置（2026-07-30）

- **状态**：已确认并用于 Phase 6.9 收尾。
- **背景**：06901–06909 已完成 C++ Target、目录、Ports/Adapters、Composition Root、
  ResourceLocator 与 Native CLI 收口；06910–06912 尝试加入 `.app`、签名与 Python-free
  发布门，但现有脚手架没有解决 OpenCV/ORT/模型/ffmpeg 自包含、manifest/SHA、许可证与
  Gatekeeper，不能作为可信发布实现。
- **决策**：
  1. Phase 6.9 的完成范围改为**开发期 Native 架构**：06901–06909 + 06913。
  2. 06910（bundle）、06911（签名公证）、06912（发布 artifact Python-free 门）整体
     后置并保持 blocked；未来发布阶段重新立项。
  3. 当前开发分支删除 bundle/signing/Python-free 发布脚本、Info.plist 模板、CMake
     bundle target 与专用 Swift bundle 路径接线，避免半成品进入 main。
  4. C++ Paddle capability 缺失时默认 fail-closed；显式
     `--runtime python` / `SUBLIFT_RUNTIME=python` 长期保留为 Oracle、benchmark 和开发回滚。
  5. 产品 manifest/SHA/下载/原子安装属于发布交付；06907 只声明开发期
     ModelBundle/ResourceLocator 与 `probe == construct`。
- **理由**：开发架构与可分发 artifact 是两套不同验收。把证书、随包依赖和干净机门混入
  日常开发会制造错误完成声明，也会让 main 长期携带未经验证的发布路径。
- **结果**：Phase 6.9 可在完整开发回归门通过后独立合入；任何“可分发、已签名、
  Python-free 产品”声明仍必须等待后置发布阶段的真实 artifact 证据。

---

## ADR-0029 Paddle 全门通过后默认 C++；build tree 自带已验收 ORT（2026-07-30）

- **状态**：已确认并由 feat-06807 实施。
- **背景**：feat-06803–06806 已证明完整算子、质量和性能达标，但产品仍处于 Python
  安全默认；同时最初 Candidate 的 `@rpath` 指向虚拟环境，依赖手工 ABI 软链，不满足
  可复现产品构建。
- **决策**：
  1. `engine=paddle` + C++ capability 的产品默认改为 C++ stable；capability 缺失只允许
     `paddle_override` 到 Python Paddle，显式/env Python 始终保留回滚。
  2. Swift 不再自行猜测模型文件集合，直接执行将启动 Worker 的
     `--probe-engine paddle`；CLI/Worker/GUI 都显示 runtime、PP-OCRv6-small 与
     stable/fallback。
  3. Paddle-enabled build 将所选 ORT 复制到 build `lib/`，可执行文件使用
     `@loader_path/../lib`（Linux 为 `$ORIGIN/../lib`）；cutover gate 校验 bundled ORT
     SHA 等于已验收官方二进制。
  4. Python Paddle 至少保留一个小版本周期；`.app` 模型/ORT 签名、公证和跨平台分发
     仍属于 6.9+，不以开发 build bundle 冒充分发完成。
- **结果**：移除虚拟环境 ABI 软链后 Paddle probe、22 个 C++ cases 与完整产品门仍通过。
  默认 C++→强制 Python→默认 C++ restart 输出 SHA exact；720s 长流完成，cancel 2.8ms，
  restart readiness 0.1ms。标准 init 10/10、Swift live/default/fallback 全绿。
- **影响**：Python/Swift runtime policy、CLI/GUI/Worker 可观测性、CMake ORT bundle、
  `check_paddle_cutover.py`、引擎矩阵和 Phase 6.8 完成状态。

---

## ADR-0028 Paddle 性能验收固定 ORT 二进制；不以输出漂移换取微基准收益（2026-07-30）

- **状态**：已确认并由 feat-06806 实施。
- **背景**：十阶段归因表明 C++ 图像前后处理并非 06805 后约 14% 回退的主因；
  Homebrew ONNX Runtime 1.28.0 动态库在同一模型上显著慢于 Python wheel 随带的官方
  1.28.0 动态库。两者版本/provider 相同但 SHA256 不同，说明版本号不足以定义性能候选。
  Rec batch=1 虽在双行微基准更快，却改变了 Latin 源最终 SRT SHA256。
- **决策**：
  1. canonical 性能门同时固定输入、模型、输出和 ORT 动态库 SHA256；Candidate 必须使用与
     Python Oracle 相同的官方 ORT 二进制，缺指纹或输出 hash 不同直接失败。
  2. 在当前 4 个性能核的平台采用 ORT intra-op threads=4；保留配置入口，不把机器特定值
     写死进算子语义。
  3. Det/Cls/Rec 归一化使用 float32 精确查表，`Ort::MemoryInfo` 与 session 同生命周期，
     避免每像素重复算术和每次调用分配。
  4. 产品 Rec batch 保持 6。任何只提升微基准、但改变端到端字幕 hash 的 batch/执行策略
     都不得进入产品默认。
  5. 性能门真实运行产品 CLI，预热后 Python/C++ 交错 3 轮取中位数，并统计完整进程树 RSS、
     OCR/box/Cls/Rec batch 数；不得使用硬编码样例指标。
- **结果**：冻结 120s canonical 视频上 Python/C++ wall median 分别为 50.700s /
  45.407s，比例 `0.8956x`（C++ 快 10.44%）；进程树 RSS 分别为 1864.406 /
  1706.297 MiB，比例 `0.9152x`。两端均为 123 次 OCR、44 条字幕且 SRT SHA256 完全一致。
  性能优化后 3 来源 614.272s 质量门仍逐源 hash exact、全部指标 delta=0。
- **影响**：`check_paddle_perf.py`、性能 manifest、Paddle options/stats/trace、
  `check_paddle_gate.py` 的逐源输出 hash 硬门，以及 Phase 6.8 cutover 的 ORT 构建约束。

---

## ADR-0027 Paddle E2E 门必须真实运行；Det 几何漂移不得靠 Cls 阈值掩盖（2026-07-30）

- **状态**：已确认并由 feat-06805 实施。
- **背景**：旧 `check_paddle_gate.py` 只比较硬编码示例指标，既不启动产品 CLI，也不检查
  输入素材、模型和输出，因此会假通过。真实三源门发现 Det quad 仅偏 1px 时，同一字幕的
  Cls 180° score 会从 Python 的 0.6868 放大为 C++ 的 0.9383，跨过 0.9 阈值并造成错误旋转。
- **决策**：
  1. Paddle E2E 门必须分别以 `runtime=python|cpp` 运行真实 `sublift extract`，统一交给
     `sublift.benchmark.diagnostics`，并同时过当前 Oracle 相对门与冻结 Python 绝对门。
  2. Manifest 必须固定来源、视频/GT/recipe/generator/font hash；缺素材、模型、worker、
     baseline 或 hash 不符一律 fail-closed。
  3. DB unclip 精确复刻 pyclipper/Clipper 6 的整数 offset、round join 与 arc tolerance，
     不以提高 Cls 阈值或放宽 CER/坐标门掩盖上游几何误差。
  4. 质量与性能分开验收；质量一致不能被性能结果替代，性能优化也必须重跑质量门。
- **结果**：3 来源共 614.272s 的 Python/C++ SRT SHA256 逐源一致；18 个相对/绝对门
  全部通过且所有聚合指标 delta=0。06805 冻结点的 C++ wall 仍慢约 14%–15%，后由
  feat-06806 / ADR-0028 解决。
- **影响**：`check_paddle_gate.py`、质量 manifest/frozen baseline、DB postprocess 回归测试
  与 Phase 6.8 cutover 证据。

---

## ADR-0026 Paddle Rec 字典与 uint8 resize 必须具有模型/算术级确定性（2026-07-30）

- **状态**：已确认并由 feat-06804 实施。
- **背景**：RapidOCR 的 PP-OCRv6 Rec 模型自带 `character` metadata，旧 C++ 却无条件读取相邻
  文本字典，存在模型与字典错配风险；同时 Python OpenCV 4.13 与 Homebrew OpenCV 4.14 的
  `INTER_LINEAR` uint8 SIMD 舍入会让同一 crop 的归一化 tensor 相差 1 个灰度级，掩盖真正的
  适配器回归。
- **决策**：
  1. Rec 字符表优先读取当前 ONNX session 的 `character` metadata；metadata 缺失时只允许
     SHA-256 白名单验证通过的外部字典，未知字典 fail-closed。
  2. Cls/Rec 的 uint8 resize 固定为 OpenCV `INTER_LINEAR` 的 11-bit coefficient 与专用
     vertical cast 算术边界，不把 tensor 结果交给随 OpenCV 小版本变化的 SIMD kernel。
  3. 以 frozen Det quad 隔离验证 Crop/Cls/Rec：输入 tensor 仍要求逐元素 `max_abs<=1e-5`，
     不因 OpenCV/ORT 构建不同放宽预处理门。
  4. frozen-quad override 只属于显式 diagnostic trace，不进入 `IOcrEngine` 或正常产品调用。
- **理由**：字典必须与模型同源；预处理是可确定的纯算子，应消除第三方二进制实现细节造成的
  假漂移。这样 ORT 推理尾数与适配器算法误差可被分别归因。
- **结果**：9 个 fixture 在固定 Python Det quad 后 crop 像素、Cls tensor、Rec tensor 均
  逐元素完全一致；180°、双行 batch、CTC token/text 与最终 AABB 全部门通过。
- **影响**：`sublift_core` 新增无第三方依赖的 SHA-256 工具；`sublift_paddle` 仍是唯一
  ORT/OpenCV 依赖边界；新增 `check_paddle_rec_parity.py`。

---

## ADR-0025 Paddle 数值门必须区分 ORT 版本与二进制构建（2026-07-30）

- **状态**：已确认并由 feat-06803 实施。
- **背景**：Det input tensor 已逐元素完全一致，但同为 ONNX Runtime 1.28.0 /
  CPUExecutionProvider 的 Python wheel 与 Homebrew C++ dylib 在 probability map 上仍有
  `max_abs=1.9729137420654297e-5`；两者 SHA256 不同。让 C++ 链接 Python wheel 的同一
  dylib 后，9 个 fixture 的 probability map 逐元素差异降为 0。
- **决策**：
  1. Paddle stage/Det 报告记录 Oracle 与 Candidate 的 ORT 动态库 SHA256，不能只记版本和
     provider。
  2. 同一 ORT 二进制使用 `probability max_abs <= 1e-5` 严格门；同版本/provider 但不同构建
     使用实测上界留裕量后的 `<=2.5e-5` 跨构建门。
  3. 跨构建只放宽推理浮点尾数；Det tensor 仍要求 exact，box precision/recall、IoU、坐标和
     score 门均不放宽。
  4. 报告必须明确 `same_binary` 与实际激活阈值，缺动态库指纹 fail-closed。
- **理由**：编译器和 kernel 构建差异会改变卷积归约尾数；把它误判为适配器算法回归会造成
  假失败，把版本号误当同一运行时又会造成假精确。最终 boxes 与文本质量仍由独立硬门约束。
- **影响**：`freeze_paddle_manifest.json`、`dump_paddle_stages.py`、
  `check_paddle_det_parity.py`、Phase 6.8 验收报告。

---

## ADR-0024 Phase 6.8 Paddle Native 质量 / 性能加固先于去 Python 分发（2026-07-29）

- **状态**：已确认（路线与验收门冻结；实现从 feat-06801 开始）。
- **背景**：6.7 已证明 C++ Paddle adapter 能加载 PP-OCRv6、接入 Worker 并输出真实 SRT，
  但其完成门主要是 synthetic 行排序/AABB golden 与 L4 文本烟测。审计发现 Candidate 使用
  简化连通域 AABB、最近邻预处理、AABB crop、逐框 Rec、缺完整 Cls/DB unclip，并在 Det
  无框时整图 Rec；现有 GT cutover 又固定为 Vision。相同 2 分钟样片 C++ wall 147.8s、
  Python 54.0s（约 2.74×），尚不具备 Paddle 产品默认条件。
- **决策**：
  1. 在原“6.8+ 去 Python/打包”前插入 **Phase 6.8 Paddle Native Quality & Performance
     Hardening**；去 Python、随包 ORT/模型/ffmpeg、universal2、公证顺延到 **6.9+**。
  2. `feat-06801` 先恢复安全路由：Paddle 自动默认 Python，只有显式 override 使用
     C++ experimental；禁止静默改为 Vision/Mock。
  3. 质量实现顺序固定为：分阶段 Oracle → 完整 Det DB/unclip → Quad crop/Cls/Rec →
     多源 Paddle E2E GT；不得用 Vision GT 或纯几何 golden 代替。
  4. **先冻结质量、后优化性能**。性能优化按 ORT 线程、Rec batch、空 Det 短路、
     OpenCV/SIMD 与缓冲复用推进，每一步重跑质量门。
  5. 产品重新 cutover 的 canonical 硬门为 C++ wall median ≤ Python ×1.20
     （目标 ×1.10），并同时满足 Paddle 专项质量、RSS、cancel/restart 与长流门。
  6. 若门未通过，Paddle 默认继续 Python；路线图不得倒逼降低阈值或强行去 Python。
- **理由**：当前差距主要来自 adapter 算法/执行策略，而不是 IPC 或 C++ 语言本身；先获得
  可归因的 stage parity，才能判断正确性与性能优化是否有效，并给默认切换提供可审计证据。
- **影响**：新增 `docs/cpp/phase6.8-paddle-hardening.md` 与 feat-06801–06807；
  `docs/cpp/NAMING.md`、引擎矩阵、Phase 跟踪和项目总览同步；分发范围改为 feat-069xx 起。

---

## ADR-0023 Phase 6.7 PaddleOCR C++ adapter 选型与路由（2026-07-29）

- **状态**：已确认（feat-06701–06706 已实现）；“可用时产品默认 C++”已被 ADR-0024
  的 6.8 安全路由 / 重新 cutover 决策取代。
- **背景**：6.6 cutover 后 vision/mock 默认 C++，paddle 仍强制 Python（rapidocr + onnxruntime + PP-OCRv6）。
  产品跨平台路径与「去 Python 产品依赖」均被 paddle 卡住；需 native adapter，且不得静默落到 vision/mock。
- **决策**：
  1. **推理栈**：C++ 使用 **ONNX Runtime + PP-OCRv6**（与 Python rapidocr 同系），**不**引入完整 PaddlePaddle Inference 训练/全栈，**不**用嵌入 CPython 调 rapidocr。
  2. **Target**：新增可选 `sublift_paddle`（`SUBLIFT_ENABLE_PADDLE` 默认 OFF）；`sublift_core` 零 ORT/ObjC 依赖。
  3. **行为 Oracle**：`src/sublift/ocr/paddle.py`；颜色（RGB 入、内部 BGR）、四角点 AABB/clamp/排序、空结果 vs 故障上抛语义必须对齐。
  4. **6.7 路由**：C++ paddle **可用**时 `engine=paddle` 允许/默认 C++ worker；
     **不可用**时显式 `paddle_override → Python`；**禁止**静默 vision/mock。审计后的
     6.8 路由与重新 cutover 以 ADR-0024 为准。
  5. **范围**：6.7 完成 adapter + worker 接线 + MVP parity/默认路由；**不**删除 Python 树、
     不在本子阶段做公证/随包模型（现顺延到 6.9+）。
- **理由**：与现有 Python extra 一致、体积可控、跨平台；延续 Phase 6「adapter 分 target + parity 再 cutover」模式（类比 6.4 Vision）。
- **影响**：`docs/cpp/phase6.7-paddle.md`、引擎矩阵、architecture target 图、`resolve_runtime` / GUI 策略、CMake 选项。

---

## ADR-0022 Cutover GT L3 固定素材缺失时的门禁豁免（2026-07-28）

- **状态**：已确认（6.6 cutover residual）。
- **背景**：engine-matrix §3.1/§3.4 与 phase6.6-cutover §4 要求固定素材 GT L3
  （F1/precision/CER/usable/noise/empty ≥ 冻结水位）或书面 waiver 后方可默认翻转。
  固定 clip `debug/Zootopia_clip_1080p.mp4` **不入库**（体积/版权），CI/多数开发机无该资产；
  水位本体已冻结于 `benchmark/baselines/quality-baseline.md`（feat-034）。
- **决策**：
  1. `scripts/parity/check_cutover_gate.py` **接线** GT L3：资产存在时用 C++ worker（vision）
     path-mode 抽帧 + 现有 `sublift.benchmark.diagnostics` 打分，硬门对齐冻结水位；跌破 → 门禁 FAIL。
  2. 资产缺失时默认 **WAIVED**（`gt_l3_waived`），报告必须显式标注豁免与
     「非完整发布契约」；**禁止**把豁免写成「全量发布契约 PASS」。
  3. 发布/合并若要求 live L3：使用 `--require-gt`（无资产即 FAIL）。
  4. 显式 `--skip-gt` 仅作开发捷径，同样记入报告 missing gates。
  5. 残差风险保留在 `progress.md`：有固定 clip 的机器应跑 live L3 再宣称质量门完整。
- **理由**：不阻塞无专有素材环境的 parity/runtime 门，同时避免假阳性「全契约通过」。
- **影响**：`check_cutover_gate.py`、`docs/reports/phase6.6-cutover-gate.md`、cutover 结论措辞。

---

## ADR-0021 Phase 6 P1 契约：Oracle、Target、IPC、引擎矩阵（2026-07-26）

- **状态**：已确认（设计冻结）；实现分属 feat-06002–06005 / 6.5–6.6。
- **背景**：6.0 起步文档方向正确，但 Oracle 若写成「当前 main」、Image 可占位后换类型、
  「同协议」忽略时序、以及「6.6 全面切 C++」与「不实现 Paddle」冲突，将在 6.1+ 返工。
- **决策**：
  1. **Oracle** 必须钉扎 `oracle_commit`、运行时版本、素材 SHA256、完整 Config、`golden_schema_version`；
     比较优先中间量（signature/events/段/代表帧/OCR 决策），规则见 `docs/cpp/parity-contract.md`。
  2. **图像** 使用 `ImageBuffer`/`ImageView` 与显式像素格式；禁止公共 API 以裸 buffer 占位后再改
     `cv::Mat`；完整 `Config` 对齐 Python；当前约束见 `docs/cpp/native-architecture.md`。
  3. **CMake targets** 拆分 `sublift_core` / `ffmpeg` / `vision_macos` / `worker` / `cli` /
     `test_support`；工具链锁定 Catch2 + nlohmann/json。
  4. **Worker 兼容** 以时序与所有权为准（progress/push_entry/entries/done、cancel、video_id），
     见 `docs/cpp/worker-ipc-contract.md`；非「消息 type 字符串相同即可」。
  5. **引擎矩阵**：6.6 后 vision/mock → C++ worker；**paddle → 仍 Python worker**；capability
     诚实暴露；禁止静默 fallback。Cutover 含质量、运行时、ASan 与 `SUBLIFT_RUNTIME` 回滚，
     当前规则见 `docs/cpp/runtime-contract.md`。
  6. **进 6.1 门槛**：`feat-06001`–`feat-06005` 全部 done。
- **理由**：把返工点前移到设计门；保留已交付 paddle，同时允许 vision 路径 native cutover。
- **影响**：强化 06002–06004；新增设计型 `feat-06005`；主 `ARCHITECTURE`/`REQUIREMENTS`/`README`
  回链 `docs/cpp/`。

---

## ADR-0020 Phase 6 Native C++ Core 与编号规则（2026-07-26）

- **状态**：已确认；子阶段 6.0 Bootstrap 启动（`feat-06001` 文档落地，实现 feat 待推进）。
- **背景**：Phase 1–5 已完成 CLI、macOS GUI、质量/性能锚、ROI 通路与 PaddleOCR 第二引擎。
  Phase 4.2 证明 Vision 请求执行主导耗时，C++ 化收益在分发与跨平台 core，而非 OCR 数量级加速。
- **决策**：
  1. 正式开启 **Phase 6 — Native C++ Core Migration**。迁移计划与设计文档放在 **`docs/cpp/`**；
     任务证据仍在 `docs/phases/phase6.json`。
  2. **策略**：保留模块边界与 SwiftUI；**UDS + JSON 暂时保留**；Python Worker → C++ Worker
    （按引擎矩阵）；`sublift_core` 不依赖 ObjC/Swift/Vision。**冻结 Oracle**，C++ 为 Candidate。
  3. **第一阶段不做**：libav、Swift↔C++ 直连、边迁边改算法、删除仓库内 Python oracle。
  4. **编号**：`feat-<PP><S><FF>`——`PP` 两位阶段（`06`）、`S` 一位子阶段（`0`=6.0）、
     `FF` 两位 feature（`01` 起）。例：`feat-06001`、`feat-06101`。规范见 `docs/cpp/NAMING.md`。
     Phase 5 的 `feat-05001` 等历史 ID 不改写；Phase 1–4 的 `feat-001~043` 并存。
  5. **6.0 范围**：文档与 P1 契约、CMake targets、models/完整 Config、parity、IPC/引擎/cutover
     设计冻结；**产品路径零切换**。进 6.1 前 `feat-06001`–`06005` done。
- **理由**：接口与 benchmark 已成熟，适合 runtime 收口；UDS 不传全帧图像，非瓶颈且利于隔离；
  子阶段可独立验收，符合 AGENTS 一次一功能。
- **影响**：新增 `docs/cpp/`、`docs/phases/phase6.json`、`feature-list` phase6 块；后续 `cpp/` 源码树。
  不改变当前 Python CLI/GUI 默认行为，直至 6.6 cutover（见 ADR-0021 矩阵）。

---

## ADR-0019 IPC OCR 引擎以服务进程绑定为准（2026-07-26）

- **状态**：已确认并完成（Phase 5 审查整改）。
- **背景**：server 启动参数已选择 OCR 工厂，但 `start_job.engine` 过去只写日志；两者不一致时，
  请求会静默以另一引擎执行。legacy frame mode 的 OCR 故障还会冒泡并关闭 UDS。
- **决策**：`--engine` 是实际 OCR 引擎的唯一权威来源。BridgeHandler 保存该名称，并在
  `start_job.engine` 不一致时返回 `done(ok=false)`；frame/finalize 的运行时故障也返回保留
  原文的 `done(ok=false)` 并清理任务状态。Swift 在解码前把 `done(ok=false)` 和协议 `error`
  统一映射为 `PipelineClientError.serverError`。
- **理由**：客户端声明与实际执行必须可观测且不可伪装；可读业务错误比 UDS 断连更可恢复，
  并允许同一连接随后显式重启任务。
- **影响**：IPC 测试必须让 mock server 与 mock 请求一致，并新增 mismatch、frame/finalize
  故障、状态清理、重启与真实 Swift UDS 回归。

---

## ADR-0018 PaddleOCR 引擎选型与接入（2026-07-26）

- **状态**：已确认并完成（feat-05001 ~ feat-05004）。
- **背景**：项目自 Phase 1 起预留 PaddleOCR 第二引擎接口（F14），Phase 4.2 归因契约明确允许
  非 Vision 引擎不实现 observer，为接入扫清协议障碍。
- **决策**：
  - 依赖栈：`rapidocr>=3.9.0,<4.0.0` + `onnxruntime>=1.16`（PP-OCRv6 small 默认，onnxruntime 后端）。
    不选 `rapidocr-onnxruntime` 1.x（停在 PP-OCRv4，已停更）。
  - 模型缓存：覆盖 rapidocr 默认 site-packages 落点为 `~/.cache/sublift/rapidocr-models`，
    跨 venv 复用，不污染 site-packages。
  - 编号规则变更：Phase 5 起 feat id 采用「阶段+序号」编码 `feat-05001` 起（`05`=Phase 5，
    `0`=小阶段 5.0，`001`=序号）；Phase 1-4 旧编号 `feat-001~043` 保持不动。
  - 不接 Phase 4.2 归因：`PaddleOcrEngine` 不实现 `timing_callback`，契约允许。
- **理由**：rapidocr 3.9 起默认 PP-OCRv6 det+rec small，中文精度优于 v4；onnxruntime 后端
  无 paddlepaddle 重依赖，pip 直装，几十 MB，跨平台。符合项目「通用可选依赖」定位。
- **影响**：新增 `ocr/paddle.py`、`pyproject.toml` paddle extra、CLI/IPC/GUI 接线；
  Pipeline 与 `OcrEngine` Protocol 保持不变。审查整改后，bridge/server 显式校验
  `start_job.engine` 与进程绑定引擎一致，并把 frame-mode 运行时故障返回为可读协议错误。

---

## ADR-0017 OCR 内部明细作为 `ocr` coverage leaf 的子树（2026-07-24）

- **状态**：已确认并完成（feat-043）。
- **背景**：Phase 4 ROI 后的 canonical 测量中，`ocr` 是主要单消费者成本，但它只有
  `OcrEngine.recognize()` 总 wall，无法区分 Vision `performRequests`、PIL→CGImage 桥接、
  request 设置、observation 映射或段内策略成本。
- **决策**：保留 `ocr` 为 core coverage 的唯一 leaf；新增的输入准备、request 设置、
  perform、observation 映射和 residual 作为 `ocr_breakdown` 子树，不进入 `stages` 或
  coverage 相加。`components + residual` 必须与 parent OCR 对账。逐段调用明细仅在 trace
  中有界输出，且不得包含文本、图像、box 或绝对路径。
- **理由**：既能解释 OCR 成本，又不会像嵌套 span 那样把同一段 wall 计入 core 两次；保持
  `OcrEngine` Protocol 不变，非 Vision 引擎可诚实降级为 opaque。
- **实测结论**：在 canonical Vision ROI 基线上，`performRequests` 请求执行约占 OCR parent
  99%；输入准备、request 设置和映射不是有效优化目标。summary 扰动为 1.319，不能作为产品
  速度基线，且根因未被本轮分块测量证明；这不影响内部归因、质量、对账与隐私均完成的结论。
- **后续决策**：下一项先扩充英文、中英混排、不同字幕位置和不同片源的 GT；随后以所有来源
  的质量门为约束，实验代表帧排序与有效 OCR 调用数，优先消除空 retry。不得直接降低共识阈值，
  也不重新打开桥接、Vision 并行或 producer/consumer 重叠方向。
- **影响**：涉及 `diagnostics/performance.py`、`ocr/vision.py`、`pipeline/core.py` 与
  benchmark 报告；不授权改变 OCR 算法或产品调度。完整证据见
  `docs/reports/phase4.2-ocr-attribution-baseline.md`。

---

## ADR-0016 未通过真实吞吐门的 path-mode overlap 不进入 main（2026-07-24）

- **状态**：已确认；feat-042 归档，main 保持串行 path mode。
- **背景**：有界 producer/consumer 实验保持 detection hash `b2d35c1e25f156e1`、固定 GT
  质量、Queue≤8、取消与重启均正确。两轮 Vision A/B 的 end-to-end median 比却分别为
  0.9559 和 1.0587，均未满足预先设定的 ≤0.95 保留门。
- **决策**：不为接近阈值继续调度微调，不把实验代码作为默认路径合入 main。保留可复核的
  原始数据，在 OCR 内部归因完成前不重新打开该优化方向。
- **理由**：ROI 后 producer 已能快速领先并反压，单消费者 Vision 主导；未验证的用户可见
  吞吐收益不足以抵消并发带来的资源所有权、取消与观测复杂度。
- **结果**：实验结论和限制记录在 `docs/phases/phase41.json`；后续 feat-043 已完成 Vision
  内部归因，下一步是多源 GT 后的代表帧排序与有效调用实验，不是第二轮并发重构。

---

## ADR-0015 性能 coverage 以排他 Pipeline 编排阶段补齐（2026-07-17）

- **状态**：已确认并已实现（feat-041）。
- **背景**：ROI A/B 的某次实测中，`frame_materialize` 已显著下降，但
  `stage_coverage_pct` 有一轮为 98.996%。约 121ms 的 core wall 未归入现有 leaf stages，
  使总耗时差异无法可靠解释；这段时间来自 `run_frames` 的帧迭代、状态机/Timeline 分派、
  容器阶段自身与 recorder 固定开销，而非 ROI 像素处理或 OCR 结果变化。
- **决策**：
  1. 新增 `pipeline_overhead` 为 coverage leaf stage；它测量 `Pipeline.run_frames()`
     的外层 wall，并扣除其中互不重叠的既有 leaf stages。
  2. `finalize` 继续作为容器 stage 排除在 coverage 外；其未嵌套编排成本自然归入
     `pipeline_overhead`，而内部 `ocr` / `dedupe` 仍仅计一次。
  3. 新增 `PerformanceRecorder.exclusive_span()` 作为通用排他计时 API；使用方必须只传入
     彼此不重叠的子阶段。
- **理由**：以“外层 wall − 内层 leaf”记录编排时间，既能使 coverage 可审计，又不改变
  ROI、打轴或 OCR 的实际执行路径；把未归因时间静默忽略或降低门槛都会削弱性能结论。
- **结果**：fake-clock 证明无双计；clean commit 1b4612b 的 canonical Vision A/B 中，ROI
  三次 coverage 为 99.999778% / 99.999784% / 99.999730%，全部硬门与软目标通过。

---

## ADR-0014 固定字幕区域自动在 ffmpeg 输出前裁剪，Pipeline 只消费 frame-local 坐标（2026-07-17）

- **状态**：已确认并已实现（feat-038/039/040）；ROI filter 为 `fps,format=rgb24,crop:exact=1`。
- **背景**：feat-037 canonical baseline 显示全帧 raw RGB 输出约 7.91 GB；其中
  extract_wait 约 24.1%、frame_materialize 约 14.8%。GUI 默认 path mode 已只发送
  视频路径和固定 region，Python 端却仍让 ffmpeg 输出完整 1920×1080 RGB，随后 Pipeline
  再裁 [0,848,1920,87] 字幕带。
- **决策**：
  1. 有效固定 region 的 GUI path mode 与 benchmark ROI 组，使用 ffmpeg
     fps,crop(...:exact=1)，只把 ROI raw RGB 经 stdout 传入 Python。
  2. source-frame region 只用于 ffmpeg 与诊断；ROI Frame 进入 Pipeline 后一律改用
     frame-local 全幅 Region [0,0,w,h]，由 RoiPassthroughDetector 明确表达，禁止把
     source box 二次用于图像 crop。
  3. GUI 不增加 ROI 开关；显式 ROI 非法时任务报错，region 为空、legacy frame mode、
     BottomCrop 与未验证旋转映射维持现有全帧路径。
  4. benchmark 保留仅供内部使用的 full / roi A/B 输出模式；ROI 结论必须与同次
     detection_hash、固定 GT 质量门和环境元数据一起报告。
  5. 本决策不宣称 codec 级 ROI decode；H.264 / HEVC 等通常仍需完整重建编码帧。
- **理由**：ROI 输出能以确定比例消除无效 RGB 搬运，却不改打轴/OCR 算法；局部坐标
  detector 使 source / ROI 坐标不会静默混用，便于测试与回退。
- **影响**：涉及 extractor、detector、pipeline、bridge 与 benchmark；CLI BottomCrop、
  JPEG/缩放、并发、旋转映射和跨片源质量泛化均不纳入本轮。完整契约见
  docs/design/roi-data-path.md。

---

## ADR-0013 SSIM patrol 为内部默认机制，不暴露给 GUI 用户（2026-07-13）

- **背景**：feat-031 A/B 验证时 GUI 接入了「SSIM 巡逻」开关。产品稳定后该开关仍留在主界面与设置页；且 Swift 关闭时发送 `nil` 而非 `false`，Python 继续用默认 `True`，开关形同虚设。
- **决策**：
  1. 删除主界面与设置页的 SSIM 巡逻开关；GUI 不再传 `enable_ssim_patrol`。
  2. 产品路径统一使用后端 `ChangePointConfig.enable_ssim_patrol=True`。
  3. IPC 可选字段保留，供 benchmark、回归测试与内部诊断显式 `true`/`false`。
  4. 若将来需要 GUI 调试入口，仅放在 DEBUG 开发者设置，且关闭时必须发送明确的 `false`。
- **理由**：SSIM patrol 是打轴质量的内部补强，不是用户可选偏好；暴露半失效开关只会制造假控制与支持成本。
- **结果**：GUI 提取始终走默认开启 patrol；诊断路径仍可显式关闭。

---

## ADR-0012 增量 pipeline、真实进度与取消共享任务生命周期（2026-07-12）

- **背景**：Phase 2 批量模式会先缓存全部帧再统一处理，用户长时间看不到字幕；取消只改变 bridge 状态，无法解除 worker 在 ffmpeg 读取上的阻塞。CLI 与 GUI 也缺少一致、真实的阶段进度。
- **决策**：
  1. `Pipeline` 以 `feed(frame)`、`ocr_segment(segment)`、`finalize()` 支持逐帧推进，段闭合后立即通过 IPC `push_entry` 推送。
  2. GUI path mode、CLI 与 benchmark 共享 Python `FfmpegExtractor`；以视频时长和采样率估算总帧，报告 `processing/finalizing` 与实际帧计数。
  3. 单次任务共享持久化 cancellation event；取消同时终止 ffmpeg 子进程，worker 在 `finally` 中释放 extractor 并回收线程，使下一任务可重新启动。
  4. Vision OCR 调用使用 `autorelease_pool`，图像桥接数据使用 CFData 生命周期，避免长流临时对象累积。
- **理由**：首条反馈、进度真实性、快速取消和内存稳定性本质上属于同一处理生命周期，必须由同一状态与资源所有权约束，不能靠 UI 假进度或仅设置布尔标记补偿。
- **结果**：自动审计首条反馈 0.68s、取消响应 0.108s，ffmpeg 进程退出且第二任务可正常启动；4K 流式内存审计通过。≥10 分钟非 Zootopia GUI 手工体验验收由用户决定暂缓。

---

## ADR-0011 OCR 文字系统默认 auto，CJK 横幅按边界清理（2026-07-10）

- **背景**：feat-034 初版为清理 `PHISON/SON` 使用无条件拉丁尾缀正则，误删纯英文和合法中英混排；其相似文本聚类又未把簇票数传给接受策略，低置信中文字幕仍被清空。
- **决策**：
  1. 无显式 `SubtitleProfile` 时文字系统默认 `auto`；GUI 按选中候选文本推断 `cjk/latin/auto`，CLI/benchmark 可显式固定。
  2. 共识结果必须携带真实 `support_votes`，低置信接受策略直接消费簇票数，不再下游按全文精确相等重算。
  3. 仅在显式 CJK 画像下清理与 CJK 边界直接粘连的拉丁横幅；纯英文、空格分隔英文及中文内部缩写均保留。
- **理由**：让语言假设来自用户选区或显式配置，以多帧证据处理 OCR 变体，并把水印清理限制在可解释的结构边界内。
- **结果**：固定 GT usable 92.0%、CER macro 3.2%、noise/empty 0、timing F1 97.7%、precision 98.8%；合法英文与 `ZPD` 有回归测试。

---

## ADR-0010 打轴抽帧统一到 Python FfmpegExtractor（2026-07-09）

- **背景**：GUI 用 AVF+JPEG 推帧时，同 region/Config 下 timing F1 ~84–86%，而 CLI/benchmark live（`FfmpegExtractor`）达 95.2%（5fps）/ 96.4%（8fps）。日志证明 region 与 pipeline 默认参数一致，差在选帧相位、时间戳网格与 JPEG 有损。
- **决策**：
  1. **打轴采样唯一实现** = Python `FfmpegExtractor`（与 CLI / `run_benchmark` 同源）。
  2. GUI 默认 **path mode**：`start_job.video_path` 传本地路径，后端自抽帧；Swift **不再**为打轴推 JPEG frame 流。
  3. 保留 **frame mode**（无 `video_path`）作兼容/调试。
  4. AVF 仅用于 **预览与选区代表帧**，与打轴解耦。
- **理由**：验收与产品必须同一像素/时间戳序列；双端各抽帧必然漂移。
- **影响**：`bridge.py` path mode；`SubtitleExtractor` 默认 path；`docs/design/macos-gui.md` 抽帧章节；后续默认 fps 仍建议 5（速度），8 作高精度档。

---

## ADR-0009 移除 Phase 2 .app 打包与 notarization 流程（2026-07-06）

- **背景**：feat-025 原计划把 SwiftUI 工程打包为可分发 `.app`，并完成 Developer ID 签名 + `notarytool` 公证，使 Gatekeeper 放行。该流程需要 Apple Developer Program 会员、Developer ID 证书、embedded Python 运行时以及 hardened runtime 适配。
- **决策**：**跳过 feat-025**，不做 `.app` 打包、embedded Python、签名与公证。Phase 2 当前范围止于「可在开发者环境通过 SwiftPM 构建并运行」的 macOS GUI，不产出面向终端用户的独立 `.app` 分发包。
- **理由**：用户明确决定移除该流程；当前阶段优先完成 GUI 功能与文档收尾，避免引入证书、公证、embedded Python 等分发侧阻塞。
- **影响**：
  - `docs/phases/phase2.json` 中 feat-025 状态改为 `blocked`，并注明跳过原因；feat-026 依赖从 feat-025 改为 feat-024。
  - `feature-list.json` 中 `phase2.distribution` 状态改为 `blocked`。
  - `README.md` 与相关文档不再包含 `.app` 安装 / 分发段，仅描述 SwiftPM 构建运行方式。
  - 未来如需分发，可重新开启 feat-025 或作为独立 release 工程处理。

---

## ADR-0008 feat-022 字幕区域：Vision 候选框 + 用户多选（2026-07-05）

- **背景**：feat-022 原计划为用户在预览上手动画矩形并重新跑流水线。用户反馈手动画框负担高、画错易导致识别失败；HURDLES 已记录 `bottom_ratio=0.3` 裁太宽问题，方案 3 为 Vision 自适应区域检测。
- **决策**：
  1. **取消手动画框**；改为代表帧上 **Vision 自动检测全部文字候选框**，预览以**彩色编号框**叠加展示。
  2. 用户**多选**哪些候选框属于字幕（可排除新闻标题等误检）；支持多行字幕对应多框。
  3. **区域推算**：**Y** 取选中框 min~max（加 padding）；**X MVP 固定全宽**（`x=0, width=video_width`）。
  4. 检测与预览叠加在 **Swift 端**用 `VNRecognizeTextRequest` 实现（feat-022a）；合并后的 `region_box` 接入 `start_job` + `bridge` `FixedRegionDetector`（feat-022b，已完成）。
- **理由**：兼顾自动化与用户可控；比纯启发式合并更抗误检；比手动画框更低门槛。X 全宽避免裁掉长字幕行。
- **影响**：`RegionPicker`(手画) 改为 `RegionOverlay`+`RegionCandidateList`；`docs/phases/phase2.json` feat-022 重写；`docs/plans/phase2.md` 区域相关段落已同步（§6.2/§6.3/§7/§8.3/§9）。

---

## ADR-0007 Phase 2 GUI 三项设计决策（2026-07-04）

Phase 2 启动前对三项影响 feat-015/016/024 的设计点拍板：

### ADR-0007a Pipeline 帧流接入：新增 `run_frames()`（feat-016）

- **背景**：Phase 1 `Pipeline.run(video_path: Path)` 内部调 `self._extractor.extract(video_path)`，假设帧来自视频文件。Phase 2 GUI 模式下帧由 Swift 端 AVFoundation/ffmpeg 抽取后经 IPC 以 JPEG 字节流送入 Python，无 `video_path`。plan §5.3 原写「小幅改造由 feat-016 评估」，未定方案。
- **决策**：在 `Pipeline` 上新增 `run_frames(frames: Iterator[Frame]) -> list[SubtitleEntry]` 方法，与 `run(video_path)` 并列。`run(video_path)` 内部重构为先用 extractor 生成 frames 迭代器再调 `run_frames`，避免逻辑重复。`bridge.py` 接收 IPC JPEG 帧后重建 `PIL.Image` 组装 `Frame`，调 `run_frames`。
- **理由**：这是真实的接口需求（GUI 帧源不是文件），「Python 核心零修改」是 Phase 2 起初的理想化约束，最小侵入的扩展方法优于 bridge 重写编排逻辑（C 方案）或假 path 包装（B 方案）。`run_frames` 与 `run` 共享内部步骤，不破坏 Phase 1 CLI 行为。
- **影响**：`src/sublift/pipeline/core.py` 新增 `run_frames` 并重构 `run`；feat-016 bridge 直接调 `run_frames`；Phase 1 测试需保持通过（`run` 行为不变）。更新 `docs/plans/phase2.md` §5.3 与 `docs/phases/phase2.json` feat-016 描述。

### ADR-0007b SRT 导出：Swift 端直接实现（feat-024）

- **背景**：feat-024 原写「经 IPC 调 Python SrtExporter 或 Swift 端直接调 sublift.export 模块，双方案内选一」。编辑后的字幕条目已存在于 Swift 内存，走 IPC 往返 Python 无收益。
- **决策**：SRT 格式化在 Swift 端实现。Swift 维护 `entries: [SubtitleEntry]` 模型，导出时本地格式化为 SRT 文本写文件。
- **理由**：SRT 格式极简（序号 + `HH:MM:SS,mmm --> HH:MM:SS,mmm` + 文本 + 空行），无理由走 IPC 往返；且避免引入「为导出再发 IPC 请求」的状态机分支。
- **影响**：`apps/macos/Sources/SubLiftMac/Core/SrtFormatter.swift` 新增；不调 Python 导出。更新 `docs/phases/phase2.json` feat-024 描述。

### ADR-0007c/d MsgPack 评估与撤销（独立决策，2026-07-05）

- **背景**：feat-015 原计划在 IPC 层引入 MsgPack 替换 JSON（plan §4.2）。评估两个 Swift MsgPack 库后，发现都有嵌套解码 bug（详见 HURDLES）。
- **决策**：**不引入 MsgPack，继续用 JSON**（ADR-0007c 撤销引入，ADR-0007d 确认 JSON 为最终方案）。
- **影响**：此决策**独立于 feat-015 任务范围**。feat-015 仍需定义 7 类消息的 JSON schema + 双向单测，只是序列化层用 JSON 而非 MsgPack。feat-015 因此从「MsgPack 序列化」重定义为「IPC 协议 7 类消息 schema（JSON 序列化）」。

---

## ADR-0006 Phase 2 抽帧双方案：AVFoundation 主路径 + 系统 ffmpeg 兜底（2026-07-03）

- **背景**：Phase 2 GUI 需要从视频抽帧后通过 IPC 送 Python Pipeline。AVFoundation 是 macOS 原生方案，可利用 VideoToolbox 硬解且无需额外依赖；但 feat-012 spike 证明 AVFoundation 无法打开 mkv 容器（报错 `-11828` / `-12847`），而 SubLift 需要支持 mkv 输入。
- **决策**：
  1. **主路径**：mp4 / mov / H.264 / HEVC 走 `AVAssetReader` + `AVAssetReaderTrackOutput`，`CVPixelBuffer` → `CGImage` → JPEG q=0.85。
  2. **兜底路径**：`.mkv` 及 AVFoundation 拒绝的容器走系统 `ffmpeg`，命令 `ffmpeg -i input -vf fps=5 -f image2pipe -vcodec mjpeg -`，stdout 为 MJPEG 流，按 SOI/EOI marker 切帧。
  3. **运行时检测**：启动时 `which ffmpeg` 检测，缺失弹窗引导 `brew install ffmpeg`，UI 禁用 mkv 拖入。
- **理由**：AVFoundation 覆盖 macOS 最常见格式且零额外依赖；ffmpeg 是 mkv 的通用解。按扩展名路由简单可靠，避免 AVFoundation 失败后再回退的 ~1s 延迟。
- **影响**：
  - `apps/macos/Sources/SubLiftMac/Core/FrameSampler.swift` 负责 AVFoundation 路径。
  - `apps/macos/Sources/SubLiftMac/Core/FfmpegFallback.swift` 负责 ffmpeg 检测、MJPEG 切帧、mkv 抽帧与元数据探测。
  - `apps/macos/Sources/SubLiftMac/Core/VideoMetadata.swift` 对 mp4/mov 用 AVURLAsset，对 mkv 用 ffprobe。
  - `docs/plans/phase2.md` §5 更新为双方案描述。

---

## ADR-0005 Phase 2 GUI 架构：SwiftUI 壳 + Python UDS 子进程（2026-07-03）

- **背景**：Phase 1 已交付可运行的 CLI Pipeline（Python）。Phase 2 需要 macOS GUI，目标是复用 Phase 1 算法核心，避免重写。
- **决策**：
  1. **双工程布局**：`apps/macos/` 新建 SwiftUI 工程，`src/sublift/` Python 工程保持不变。
  2. **进程边界**：SwiftUI 主进程作为 UI 壳，通过 **Unix Domain Socket (UDS)** 与本机启动的 Python 子进程通信。
  3. **Phase 1 核心零修改**：`pipeline/` / `extractor/` / `detector/` / `ocr/` / `export/` / `models.py` / `config.py` 不改动；新增 `src/sublift/ipc/` 模块把 Pipeline 包装为 IPC handler。
  4. **序列化**：JSON（4 字节大端长度前缀分帧），见 ADR-0007c/d。
- **理由**：
  - 复用已验证的 Python 算法核心，降低 GUI 阶段风险。
  - UDS 是本地进程间通信的轻量方案，不依赖网络，Swift 与 Python 都原生支持。
  - 明确的分层边界为 Phase 3 跨平台留路：核心算法与 UI 壳解耦，未来可用 Tauri/Electron 替换 SwiftUI 而不动 Python 核心。
- **影响**：
  - `apps/macos/Sources/SubLiftMac/Core/PipelineClient.swift` 负责启动 Python 子进程与 UDS 通信。
  - `src/sublift/ipc/server.py` / `protocol.py` / `bridge.py` 负责 Python 端 IPC 服务。
  - `Pipeline.run_frames()` 新增（ADR-0007a），让 bridge 可以注入 Swift 送来的 JPEG 帧流。
  - `docs/ARCHITECTURE.md` 增加 Phase 2 章节描述双工程 + IPC。

---

## ADR-0004 任务粒度调整：22 细任务合并为 10 粗任务（2026-07-03）

- **背景**：初始 Phase 1 规划拆出 22 个细粒度任务（feat-001~022），实践中发现粒度过细，导航与跟踪成本高。
- **决策**：合并为 10 个粗任务（feat-001~010），每个粗任务 = 一个可独立验收的功能块；任务内部细节通过 phaseN.schema.json 新增的 `subtasks` 字段承载（name + description），不拆成独立任务。CLI extract 实现随 feat-009/010 接入；端到端真实视频验收作为 Phase 1 级验收门，不单列任务。feat-004（文档）移到末尾依赖 feat-010，确保实现稳定后再写文档。
- **理由**：粒度以「能否独立验收」为准；粗任务承载主验收，subtasks 承载实现细节，兼顾可跟踪性与导航效率。
- **影响**：`docs/phases/phaseN.schema.json` 新增 `subtasks` 字段；`docs/phases/phase1.json` 重写为 10 任务 + 17 subtasks；`feature-list.json` covers 对齐；`docs/plans/phase1.md` 任务表与执行顺序图重写。ADR-0001/0003 中对旧 task ID 的引用以本决策为准。

## ADR-0001 Phase 1 模块布局与分层（2026-07-03）

- **背景**：Phase 1 需建立项目底座，定义字幕提取的三能力模块与串联层。
- **决策**：采用「三能力模块 + 串联层 + 入口层」分层。
  - 能力模块：`detector`（字幕区域检测）、`extractor`（帧采样）、`ocr`（OCR，可插拔多引擎）。每个模块定义 Protocol + 默认实现，可热插拔。
  - 串联层：`pipeline`（端到端编排，含 timeline 变化点状态机与 dedupe 去重合并）、`export`（字幕导出，Phase 1 实现 SRT，ASS/VTT 留接口占位）。
  - 入口层：`cli`（argparse，零依赖起步，`sublift extract` 子命令）。
- **理由**：用户明确三模块划分（detector/extractor/ocr）；extractor 定位为「帧采样器」而非端到端编排器；串联逻辑由 pipeline 承担。模块低耦合高内聚，平台特定 API 只能出现在 `ocr/vision.py`，核心层只依赖 Protocol，为 Phase 3 跨平台留路。
- **影响**：`docs/plans/phase1.md` §2 模块布局、§5 抽象接口；`feature-list.json` 7 大功能块；`docs/phases/phase1.json` 任务跟踪（任务粒度后经 ADR-0004 调整）。

## ADR-0002 Apple Vision 经 PyObjC 桥接接入（2026-07-03）

- **背景**：Phase 1 需选定 OCR 默认引擎的接入路径。
- **决策**：Python 通过 `pyobjc-framework-Vision` 直接调用 `VNRecognizeTextRequest`，不引入 Swift helper 子进程。
- **理由**：Phase 1 是 CLI 核心定位，纯 Python 路径最快；PyObjC 桥接避免额外的构建/分发复杂度；Swift 留给 Phase 2 GUI。
- **影响**：`pyobjc-framework-Vision` 与 `pyobjc-framework-Quartz` 作为 macOS 可选依赖；`ocr/vision.py` 是平台特定 API 唯一容身处；导入失败需优雅降级提示。

## ADR-0003 Phase 1 范围确定为可运行 MVP（2026-07-03）

- **背景**：Phase 1「底座」的深度需明确，决定验收标准与工作量。
- **决策**：Phase 1 交付可运行 MVP——真实 1080p 视频经 `uv run sublift extract <video> -o out.srt` 产出可加载 SRT；而非仅抽象接口。
- **理由**：用户选择「可运行 MVP」选项。端到端可运行才能验证架构有效性，避免抽象底座脱离实际。
- **影响**：`docs/phases/phase1.json` 验收标准含真实视频产出 SRT 与三工具全绿；端到端验收作为 Phase 1 级验收门（任务粒度见 ADR-0004）。ASS/VTT、PaddleOCR 第二引擎、配置文件、进度展示等显式排除。
