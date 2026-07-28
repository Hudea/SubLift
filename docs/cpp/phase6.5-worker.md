# Phase 6.5 — C++ Worker（UDS + JSON 协议兼容）

> 子阶段编码：`S = 5` → feature 前缀 `feat-065xx`  
> 任务跟踪：`docs/phases/phase6.json`  
> 总览：[phase6-overview.md](phase6-overview.md)  
> 契约：**[worker-ipc-contract.md](worker-ipc-contract.md)**（权威）· [engine-matrix-and-cutover.md](engine-matrix-and-cutover.md) · [architecture.md](architecture.md)  
> **门槛：** `feat-06401`–`feat-06405` 全部 `done`（已满足）  
> **Oracle：** `src/sublift/ipc/`（`protocol.py` · `server.py` · `bridge.py`）+ Swift `PipelineClient` 时序

## 1. 目标

在 **不切换产品默认 runtime** 的前提下，实现可被现有 macOS GUI 驱动的 **`sublift_worker` 可执行文件**：

```text
Unix Domain Socket
  ←→ 4-byte BE length + UTF-8 JSON
        → hello/bye (+ capability)
        → start_job (path | frame)
        → progress / push_entry / log*
        → entries | done(ok=false) | error
        → cancel_job / 断连回收
```

内部装配 **6.1–6.4 已有积木**：

```text
plan_frame_io + FfmpegExtractor
    → Pipeline(feed / ocr_segment / finalize / cancel)
        → IOcrEngine = Mock | Vision（按 --engine / 编译门）
```

| 交付 | 说明 |
|---|---|
| 分帧编解码 | 与 Python `read_message` / `write_message` **字节兼容** |
| 协议消息 | 字段名对齐 `protocol.py` / Swift `Messages.swift` |
| Capability 握手 | `runtime=cpp` + 诚实 `engines[]`（无静默 fallback） |
| Path mode | GUI 默认：worker 内 ffmpeg 抽帧 + 流式 push + 最终 `entries` |
| Frame mode | 兼容路径：JPEG frame 流 + `finalize`（可次优先） |
| Cancel | 杀 ffmpeg + `Pipeline::cancel`；资源可回收 |
| 夹具 / 冒烟 | 消息序 golden 或录制回放；**产品默认仍 Python worker** |

**本子阶段结束时：**

- 可用 **开发者/双轨** 方式启动 C++ worker（环境变量 / 配置 / 文档化 launch path）。
- **GUI/CLI 默认仍可不改**；**默认 cutover 归 6.6**。
- **Paddle 不进 C++ worker**（capability 不谎报）；选 paddle 仍走 Python worker。

## 2. 做 / 不做

### 做

1. 替换 `cpp/src/worker/main.cpp` 版本打印壳为真实 UDS 服务循环。
2. **分帧**：`>I` 长度前缀 + JSON body；超限 / 坏 JSON 的失败语义对齐契约。
3. **握手**：`hello` → `bye` 带 `protocol_version`、`runtime`、`engines`、`capabilities`（见契约 §2）。
4. **`BridgeHandler` 等价物**：path mode 主路径；frame mode 至少可验收或明确分期。
5. **引擎诚实**：进程启动绑定 `--engine`（或等价）；`start_job.engine` 不一致 → `done(ok=false)`，禁止静默换引擎。
6. **引擎矩阵（6.5 实现集）：** `mock` 必达；`vision` 在 `SUBLIFT_ENABLE_VISION=ON` 时可达；**无 `paddle`**。
7. **装配：** `plan_frame_io` + extractor + detector + `Pipeline` + OCR；进度估算与 Python 同量级策略。
8. **`push_entry` / `progress` / `entries` 时序** 满足 Swift `requestStreaming`（契约 §4）。
9. **cancel / 断连：** 协作取消 + kill ffmpeg + 抑制迟到 push（best-effort）。
10. 夹具：至少 path mode + mock 的 **消息类型序** L0 比对；可选录制 Python 一轮作 oracle。

### 不做

| 项 | 归属 |
|---|---|
| 产品默认切 C++ worker / 删 Python worker | **6.6** |
| 原生 Paddle 进 C++ | 引擎矩阵；禁止谎报 capability |
| libav | 禁止 |
| 改打轴/OCR 算法「适配」IPC | 禁止 |
| 重写 SwiftUI（仅允许文档化可选 launch path） | 6.6 或独立小 feat |
| 完整 GT L3 cutover 门 | **6.6** |
| 随 app 签名公证 / universal2 打包 | **6.7+** |

## 3. 任务一览

| ID | 名称 | 验收一句话 |
|---|---|---|
| **feat-06501** | 分帧编解码（无业务） | `>I`+JSON 读写单测；超限/截断行为文档化 |
| **feat-06502** | 协议类型 + capability 握手 | 构造/解析业务消息；hello→bye 字段齐全诚实 |
| **feat-06503** | 连接循环 + path mode（Mock） | UDS 上 start_job(path)+mock → progress* + entries；引擎不匹配失败 |
| **feat-06504** | Cancel / 断连 / frame mode 最小集 | cancel 杀 ffmpeg；断连回收；frame+finalize 或明确 defer 并测 stub 拒绝 |
| **feat-06505** | Vision 装配 + IPC 夹具 / worker CLI | `--engine vision`（ON 时）；fixture 消息序；init/文档双轨启动 |

依赖：

```text
feat-06405 + 6.2 Pipeline + 6.3 Extractor
    └── feat-06501 framing
            └── feat-06502 protocol + capability
                    └── feat-06503 path mode mock e2e
                            └── feat-06504 cancel + frame min
                                    └── feat-06505 vision + fixtures + CLI docs
```

**推荐一次一刀：** 06501 → 06502 → 06503 → 06504 → 06505。

---

## 4. 任务详述

### feat-06501 — 分帧编解码

**Python oracle：** `server.py` `read_message` / `write_message`（`struct >I`、`MAX_MESSAGE_BYTES`）。

| 必须 | 说明 |
|---|---|
| API | 例如 `write_framed(fd/socket, json)` / `read_framed` |
| 上限 | 与 Python 对齐或文档化同一常量 |
| 错误 | 长度 0/超限 → 错误；短读 → 对端关闭 |
| 单测 | 圆跳（encode→decode）；恶意长度；空 body |
| 位置 | `sublift_worker` 内或小静态库 `sublift_ipc`（**勿**把 UDS 细节塞进 `sublift_core` 公共 API） |

**不做：** accept 循环、业务 handler。

---

### feat-06502 — 协议 + capability

**Python oracle：** `protocol.py` 各 `build_*` / 校验；契约 §2–3。

| 必须 | 说明 |
|---|---|
| 类型常量 | `start_job` / `frame` / `finalize` / `cancel_job` / `progress` / `push_entry` / `entries` / `log` / `done` / `error` / `hello` / `bye` |
| 解析 | 必填字段缺失 → 可映射 `error` 或 `done(ok=false)` |
| Capability | `runtime:"cpp"`；`engines` 仅真实可用；`capabilities` 含 `path_mode` 等 |
| nlohmann | Worker 目标可用 json；**不**强迫 `sublift_core` 公开展示 |
| 单测 | 表驱动 JSON 样例（可从 Python dump fixtures） |

**不做：** 跑 Pipeline。

---

### feat-06503 — 连接循环 + path mode（Mock）

**Python oracle：** `handle_connection` + `BridgeHandler` path 分支。

| 必须 | 说明 |
|---|---|
| Listen | UDS path CLI 参数（对齐 `serve_once` / server argparse 语义子集） |
| 一连接一 job | 进行中再 `start_job` → 明确错误 |
| Path 装配 | `plan_frame_io` → extractor → Pipeline；OCR = Mock（固定或可配置） |
| 输出时序 | `progress`（含 ready）→ 可选 `push_entry` → **`entries` 主成功**；`done(ok=true)` 可选 |
| Engine 绑定 | 启动 `--engine mock`；请求 vision → `done(ok=false)` 文案可观测 |
| 线程模型 | **单线程**串行处理消息 + OCR（与架构一致）；path 长任务可用 worker 线程但 **socket 写须串行化** |
| 集成测 | 本机 UDS 回环；合成短视频 + Mock |

**不做：** cancel 全套（06504）；Vision（06505）；产品默认切换。

---

### feat-06504 — Cancel / 断连 / frame mode 最小集

| 必须 | 说明 |
|---|---|
| `cancel_job` | 设标志；`FfmpegExtractor::cancel` / kill；`Pipeline::cancel`；可观测结束 |
| 断连 | 对端关 socket ≈ 取消 + `finally` 回收 |
| 迟到消息 | cancel 后 best-effort 不再 `push_entry` |
| Frame mode | **最小**：收 JPEG → RGB decode → feed；`finalize` → entries；**或** 明确返回「frame mode 未实现」且 path 不受影响（若 defer，须在 06504 evidence 写明并保留测试锁） |
| 重启 | 同连接或新连接可再 `start_job`（无串扰） |

**建议：** path 优先；frame 若工期紧可 stub，但 **不得** 静默当 path 处理。

---

### feat-06505 — Vision 装配 + 夹具 + CLI 文档

| 必须 | 说明 |
|---|---|
| Vision | `SUBLIFT_ENABLE_VISION=ON` 且 `--engine vision` 时 capability 含 vision；装配 `VisionOcrEngine` |
| 夹具 | `benchmark/parity/goldens/ipc/` 或 `cpp/tests/ipc/fixtures/`：path+mock **消息 type 序列** L0 |
| Dump | 可选 `scripts/parity/dump_ipc_session.py` 录 Python worker 一轮作 oracle |
| CLI | `sublift_worker --socket PATH --engine mock|vision`（字段对齐 Python server 子集） |
| 文档 | `cpp/README` + 可选 macOS：如何 **opt-in** 指到 C++ binary（**默认仍 Python**） |
| init | 不强制启动 GUI；可加 worker 冒烟（listen+hello+bye）或 fixture 单测 |

**比较层：**

| 对象 | 层 |
|---|---|
| 消息 `type` 序、`done.ok`、engine mismatch 文案关键部分 | **L0** |
| Mock path 下 `entries` 文本/时间戳 | **L0**（可控） |
| Vision path 文本 | **L4**（不作为 6.5 硬门） |
| cancel ≤1s / 重启 ≤5s | 记录式；硬门 **6.6** |

---

## 5. 建议模块与 API 方向

```text
cpp/src/worker/
  main.cpp                 # CLI parse + serve
  framing.hpp/.cpp         # length-prefix
  protocol.hpp/.cpp        # message DTOs + parse/build
  connection.hpp/.cpp      # accept loop, serialize writes
  bridge.hpp/.cpp          # job state machine
  engine_factory.hpp/.cpp  # mock / vision
cpp/tests/
  worker_framing_test.cpp
  worker_protocol_test.cpp
  worker_path_mode_test.cpp   # UDS loopback
  worker_cancel_test.cpp
  parity/ipc_parity_test.cpp
scripts/parity/
  dump_ipc_session.py         # optional
benchmark/parity/goldens/ipc/
  path_mock_session.v1.json
```

### 进程与 CLI（方向）

```text
sublift-worker --socket /tmp/sublift-xxx.sock --engine mock
sublift-worker --socket … --engine vision   # requires VISION=ON build
```

- 退出：socket 清理（unlink）、子进程 wait。  
- 日志：stderr；可选 `log` 消息（客户端可忽略）。

### 与 Swift 的关系

```text
PipelineClient
  → 仍讲同一 UDS/JSON
  → 6.5：开发者可改 worker 可执行路径指向 build/cpp/sublift_worker
  → 6.6：默认路径切换 + 回滚开关
```

**理想：零 Swift 协议改动**；capability 新字段旧客户端忽略。

---

## 6. 错误与引擎诚实（钉死）

| 场景 | 行为 |
|---|---|
| `start_job.engine` ≠ 进程绑定引擎 | `done(ok=false)` + 明确 message |
| 请求 paddle | capability 无 paddle → 失败；**禁止**改跑 vision |
| 文件不存在 / plan 失败 | `done(ok=false)` 用户可操作文案 |
| 无效 JSON / 超限 | `error` 或关连接（与 Python 同级） |
| Vision 构造失败 | 启动失败或 job 失败，不静默 mock |

---

## 7. 风险

| 风险 | 处理 |
|---|---|
| 异步 Python vs 同步 C++ 事件循环 | 单写者队列；path 任务可后台线程，写 socket 回主循环 |
| JPEG frame decode | 仅 frame mode；用最小解码（ImageIO/stb）并测，或 defer frame |
| 消息字段全集漂移 | 以 `protocol.py` + Swift 测试为权威；C++ 锁常用子集 + 忽略未知键 |
| 与 6.3 public API 卫生债 | worker 内可 pimpl；不阻塞 6.5 行为门 |
| 误切默认产品路径 | 文档 + 默认 launch 仍 Python；无 6.6 前改 GUI 默认 |

---

## 8. 6.5 完成定义

- [ ] `feat-06501`–`feat-06505` 全部 `done` + evidence  
- [ ] path mode + mock 经 UDS 得到与契约一致的成功时序；engine mismatch 失败  
- [ ] cancel/断连可回收；无 libav；capability 诚实  
- [ ] Vision 在 ON 构建下可 opt-in（或 evidence 说明平台限制）  
- [ ] 夹具/单测绿；`./init.sh` 不因 worker 破坏默认门  
- [ ] **产品默认仍 Python**（双轨文档化）  
- [ ] `feature-list` 6.5 块 covers 填齐  

---

## 9. 与 6.6 的边界

| 6.5 | 6.6 |
|---|---|
| C++ worker **可运行、可测、可 opt-in** | **默认** vision/mock → C++ |
| 协议兼容 | cutover 门：L3 GT、wall/RSS、回滚开关 |
| paddle 不在 C++ | paddle **显式** Python worker |
| 不要求改默认 GUI | release note + 默认 launch 切换 |
