# Phase 6 C++ 迁移质量审查报告

- **审查日期：** 2026-07-28
- **审查基线：** `2fce828`（工作区另有运行门禁生成的 `docs/reports/phase6.6-cutover-gate.md` 更新）
- **范围：** C++ core / FFmpeg / Vision adapter / Worker IPC、CLI 与 SwiftUI 双 Worker 切换、Parity 与 Cutover 门禁。
- **结论（审查基线）：** **有条件接受（Conditional Acceptance）**。模块迁移与 L0–L2 parity 的完成度很高，vision/mock 默认走 C++、paddle 显式保留 Python 的产品边界合理；当时不能宣称“全量验证已绿”或“完整发布契约已满足”。

## 修复回填（2026-07-28）

审查中列出的测试、协议、输入防护与文档漂移已由 `feat-06606` 修复；标准开发验收现已通过。发布验收仍为有条件接受：live GT L3 依 ADR-0022 豁免，独立 ASan/UBSan 构建在本机 Homebrew OpenCV 4.14 / TBB 2023.1.0 组合上于**进程退出**时崩溃，必须在发布环境解决后才可宣称完整 cutover。

| 项目 | 回填结果 |
|---|---|
| E2E 视频夹具 | 改用 core lavfi `testsrc`，检查 ffmpeg 返回码和文件；取消测试先等 `ready`。Worker E2E 9/9 通过。 |
| IPC 关闭与 capability | `bye` 后取消/清理并关闭连接；握手仅宣告绑定引擎；新增 `hello → bye → EOF` 进程测试。 |
| frame / 协议防护 | C++ 与 Python 同步限制 JPEG base64/原始字节、JPEG 签名、ImageIO 像素数与溢出；拒绝非法 fps、box 与 profile 几何。 |
| 标准验证 | ruff、mypy `src tests`、pytest（508 selected，23 integration deselected）、CTest 152/152、Swift 30 XCTest + 139 Swift Testing、`./init.sh` 均通过。 |
| sanitizer | **未通过（发布阻断）**：带 `SUBLIFT_SANITIZE=ON` 的 `sublift_worker --version` 退出码 134；栈在 `tbb::detail::r1::__TBB_InitOnce::~__TBB_InitOnce()`。 |

## Sanitizer 复核（2026-07-29，feat-06607）

此前 `SUBLIFT_ENABLE_OPENCV=OFF` 会把 `pipeline.cpp` 从 `sublift_core` 排除，却仍令
Worker 的 bridge 引用 `Pipeline`，因此连最小隔离构建也无法链接。本次将该配置改为
**诊断 Worker**：它可握手，但返回 `engines=[]`、`capabilities=[]` 并拒绝所有作业；它不是
产品 runtime，也不构成 OpenCV 发布豁免。

| 对照 | 动态依赖与结果 |
|---|---|
| ASan + `SUBLIFT_ENABLE_OPENCV=OFF` | `otool -L` 无 OpenCV/TBB；`sublift_worker --version` 退出 **0**；真实 UDS `hello` 与拒绝 `start_job` 均通过。 |
| ASan + OpenCV 4.14 | Worker 链接 OpenCV，OpenCV 链接 Homebrew TBB 2023.1.0；相同 `--version`（含 `detect_leaks=0`）退出 **134**，栈在 `tbb::detail::r1::__TBB_InitOnce::~__TBB_InitOnce()+0x38`。 |

这排除了 Worker 处理视频、IPC 会话、Pipeline 业务逻辑及 LeakSanitizer 开关作为该复现的
直接触发条件；问题被收窄到 OpenCV/TBB 被加载后的退出清理链。反汇编显示该 TBB 析构路径会
进入 `governor::release_resources()` 并经 `destroy_system_topology_ptr` 间接调用；无效地址的
根因仍需用同工具链重建/替换 TBB 或 OpenCV 后才能定论。因此 ASan/UBSan 仍是**未通过的发布门**，
不能据此声称内存安全已验收。

## 1. 总体评价

| 维度 | 评价 | 依据 |
|---|---|---|
| 架构分层 | 良好 | `sublift_core` 与 FFmpeg、Vision ObjC++、Worker 明确分 target；公共图像边界使用 `ImageBuffer` / `ImageView`，未把 `cv::Mat` 泄漏进核心 API。 |
| 行为迁移 | 良好 | 配置、signature、changepoint、timeline、dedupe、line select、pipeline、extractor、Vision 和 IPC session 均有冻结 Oracle / golden，比只比 SRT 更可靠。 |
| Runtime 路由 | 良好 | vision/mock → C++、paddle → Python；`SUBLIFT_RUNTIME=python` 回滚明确，未发现静默降级到其他 OCR 引擎。 |
| Worker / IPC | 需整改 | 主路径可工作，但存在 `bye` 生命周期、frame-mode 资源上限和 capability 语义偏差。 |
| 验证可信度 | 不足 | CTest、Swift 测试通过；标准 Python suite 当前 2 例失败；live GT L3 仍是 waiver，ASan/UBSan 本次未能独立复验。 |

迁移没有演变为机械翻译：C++ 保持原先能力模块与编排层的边界，FFmpeg 仍通过 subprocess，Vision 仍局限在 ObjC++ adapter。这是当前方案最值得保留的部分。下一步不建议重构为单进程 Swift↔C++ 或引入 libav；应优先把 Worker 的协议与验证闭环做实。

## 2. 实际验证证据

| 检查 | 结果 | 说明 |
|---|---|---|
| `uv run --extra vision --extra paddle ruff check .` | 通过 | 0 error。 |
| `uv run --extra vision --extra paddle mypy src tests` | 通过 | 70 个文件，strict 无问题。 |
| `cmake --build build/cpp && ctest --test-dir build/cpp --output-on-failure` | 通过 | 151/151 C++ 测试通过，含 core / extractor / worker / parity。 |
| `cd apps/macos && swift test` | 通过 | 30 XCTest + 139 Swift Testing 用例通过；覆盖 C++ / Python Worker 启动与 paddle 强路由。 |
| `uv run --extra vision --extra paddle pytest` | **失败** | 495 通过、2 失败、23 deselected；失败均位于 `tests/ipc/test_cpp_worker.py` 的 path-mode / cancel 用例。 |
| Cutover gate | 通过但范围有限 | 10 个 parity checks 与 runtime wall/cancel/restart/RSS 通过；`gt_l3_live_measurement` 依 ADR-0022 处于 waived，不能代表完整质量发布门。 |
| ASan/UBSan | 未复验 | 新建 sanitizer 构建在依赖 FetchContent 网络受限时无法配置；现有 `init.sh` 也未将 sanitizer 作为必跑门。 |

因此，`docs/reports/phase6.6-cutover-gate.md` 的 PASS 只能说明它自身执行的 parity/runtime 子集通过，不能覆盖完整 `pytest` 基线，也不能替代 live GT L3。

## 3. 已确认问题

### P1 — 标准验证被不具可移植性的 IPC 视频夹具阻断（已修复）

`tests/ipc/test_cpp_worker.py` 的 `generate_synthetic_video()` 使用 `drawtext`，却在等待子进程后直接返回路径，没有检查退出码或文件存在性。当前 ffmpeg 8.1.2 不含该 filter，视频没有生成；Worker 因而正确返回“视频文件不存在”，测试却把它误判成 path/cancel 回归。

- 证据：`tests/ipc/test_cpp_worker.py:48-69`；本机完整 pytest 为 **495 passed, 2 failed**。
- 影响：`init.sh` 虽然把该进程级测试设为 fail-not-skip，随后仍会继续运行并生成单项 PASS 的 cutover 报告；最终标准启动路径不绿，Phase 6 的“21/21”证据不可复现。
- 建议：mock OCR 不需要画出文本，改用无 `drawtext` 的 `color` 或 `testsrc` fixture；必须断言 `returncode == 0` 和输出文件存在。若坚持 drawtext，先探测 filter，不可用时明确 skip 并给出原因。取消用例也应等待 `ready/processing` 后再发送 cancel，避免把初始化失败当作取消语义。

### P1 — C++ Worker 未按协议处理客户端 `bye`（已修复）

Python bridge 收到 `bye` 后返回 `None`，server 随即结束连接；C++ `BridgeHandler::handle()` 对 `ByeMsg` 落入默认分支并返回空响应，而 `WorkerConnection::run()` 继续读下一帧，没有关闭连接。

- 证据：`src/sublift/ipc/bridge.py:174-179`、`cpp/src/worker/connection.cpp:37-59`、`docs/cpp/worker-ipc-contract.md:41-57`。
- 影响：当前 GUI `stop()` 会直接关闭 fd，主产品路径通常不显现；但协议兼容性、客户端资源回收和其他 IPC 客户端会出现不一致或等待 EOF 的风险。
- 建议：在 connection loop 识别 `ByeMsg` 后 break（并执行既有 `cancel_job()` 清理）；添加 Python↔C++ 共用的 `hello → bye → EOF` 夹具测试。

### P1 — C++ legacy frame mode 缺少 Python 已有的解压防护（已修复）

Python bridge 对 JPEG payload 和解压像素分别设有 `MAX_JPEG_BYTES=20 MiB`、`MAX_IMAGE_PIXELS=50_000_000`；C++ frame mode 仅受 64 MiB JSON frame 上限约束，base64 解码后直接由 ImageIO 创建完整 RGBA buffer。

- 证据：`src/sublift/ipc/bridge.py:47-48` 对比 `cpp/src/worker/bridge.cpp:33-120`；协议又明确保留 frame mode（`docs/cpp/worker-ipc-contract.md:47-50`）。
- 影响：恶意或损坏的高压缩图片可导致过大内存分配；这虽然不是默认 path mode，却仍是公开支持的本地 UDS API。
- 建议：在 base64 解码前限制编码体大小；使用 ImageIO metadata 检查宽高、像素数和乘法溢出后再分配；只接受协议声明的 JPEG；补充超大 payload、超大尺寸和畸形图片测试。

### P2 — capability 宣告与“进程绑定引擎”语义不一致（已修复）

Worker 启动时由 `--engine` 绑定单一引擎，`start_job.engine` 不一致会失败；但 `EngineFactory::supported_engines()` 会在 Vision 可用时同时返回 `vision` 与 `mock`。于是一个 `--engine mock` 的进程可能在握手中宣称 vision 可用，随后又拒绝 vision 请求。

- 证据：`cpp/src/worker/engine_factory.cpp:12-38`，与 `docs/cpp/worker-ipc-contract.md:35-37` 的“只展示 capability 中 engines”相冲突。
- 建议：`engines` 表示当前进程可实际接受的集合时，应只返回 `bound_engine`；若需要表达“可通过重启获得”的引擎，新增独立字段，避免复用 capability 造成歧义。

### P2 — 主架构、启动脚本与当前 cutover 状态存在文档漂移（已修复）

README 和 runtime policy 已把 vision/mock 默认切到 C++，但主架构仍写“当前产品默认路径仍是 Python”，并把 Phase 6 标为进行中。`init.sh` 也把 CMake 缺失视为可选 skip，与 C++ 已是默认产品路径不一致；同时它只运行 `mypy src`，低于项目说明中的 `mypy src tests`。

- 证据：`docs/ARCHITECTURE.md:9-22`、`README.md` 的 Native CLI / Runtime Matrix、`init.sh:184-243`。
- 建议：以 runtime matrix 为单一事实源更新主架构；将 C++ 工具链设为默认路径的必需验证（或显式提供 `--python-only` 模式）；把 `mypy src tests` 固化进 init。

### P2 — live GT L3 与 sanitizer 仍未形成默认可复现门（仍待发布环境收口）

GT L3 已正确接入，但固定视频不入库时默认 waiver；因此当前只有 mock/intermediate parity 和运行时 smoke，尚无本机可复跑的真实 Vision 质量结论。ASan/UBSan 在设计中是 cutover 门，但标准启动脚本没有执行它。

- 证据：`docs/DECISIONS.md` ADR-0022、`docs/cpp/engine-matrix-and-cutover.md:49-73`。
- 建议：把 GT 素材以受控本地 cache + manifest SHA256 提供给 release runner，并在发布任务使用 `--require-gt`；为 macOS 增加独立的 ASan/UBSan job（可允许开发 init 不跑，但不得从 release gate 缺席）。

### P3 — 启动与协议输入的防御性验证可补强

`main.cpp` 将 socket 路径直接 `strncpy` 到 `sockaddr_un.sun_path`，未拒绝超长路径，可能出现静默截断；protocol 也未统一校验 `fps > 0`、box 正尺寸与 profile 的完整几何约束。当前 CLI / GUI 正常输入不会触发，但这类边界应在 Worker API 入口 fail-closed。

## 4. 推荐架构收口方式

保持现有 target 切分与 UDS，不建议扩大迁移范围。建议将 Worker 内部整理为两个明确对象：

```text
WorkerConnection  — framing、协议校验、写序列化、连接关闭
      │
      └─ JobSession — engine/detector/extractor/pipeline 所有权、状态机、cancel/join、终态
```

`JobSession` 应只暴露 `start_path`、`feed_frame`、`finalize`、`cancel_and_join`，并持有显式终态（Idle / Running / Cancelling / Finished / Failed）。这样可把现在分散在 `BridgeHandler` 的 atomic flag、资源释放和消息抑制集中起来，也能让 `bye`、断连、cancel 和 restart 共享同一清理逻辑。

协议层建议保留三端实现，但新增版本化的跨语言 conformance vectors：有效/无效 message、`hello→bye→EOF`、engine mismatch、path 成功、frame 成功、cancel、断连。现有 IPC session golden 是很好的起点，扩展后能防止 Python、Swift 和 C++ 的字段/状态机漂移。

## 5. 建议执行顺序

1. 修复 `tests/ipc/test_cpp_worker.py` 夹具并恢复 `./init.sh` / `pytest` 全绿；更新 Phase 6 的验证证据，撤销“21/21 全绿”的失效表述。
2. 修复 `bye`、frame-mode 资源上限与 capability 语义，新增跨语言协议夹具；这应作为独立的 Worker hardening feature。
3. 同步 `ARCHITECTURE`、`init.sh` 与 runtime matrix，明确默认 C++ 路径的工具链与类型检查要求。
4. 在有固定 GT 资产的机器运行 `check_cutover_gate.py --check --require-gt`，并在 release CI 运行 ASan/UBSan；两者通过后再把结论从“有条件接受”提升为“完整 cutover 验收”。

## 6. 不建议在本轮补的内容

- 不要为了迁移再改打轴、行选择或 Vision OCR 算法；冻结 golden 的边界是正确的。
- 不要在没有独立需求时实现原生 Paddle、libav 或 Swift↔C++ 单进程互操作；它们会放大当前尚未收口的 Worker 生命周期问题。
- 不要复制 Python benchmark 诊断到 C++；继续由 C++ 导出结果、复用既有 benchmark 是更稳妥的职责划分。
