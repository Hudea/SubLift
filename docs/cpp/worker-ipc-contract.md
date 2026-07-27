# Worker / IPC 契约（Swift ↔ Worker）

> 状态：设计冻结文档；**`feat-06005` 验收本文件**，`feat-065xx` 实现时按本文测试。  
> 源码参考（行为 oracle）：`src/sublift/ipc/*`、`apps/macos/.../PipelineClient.swift`。  
> **「消息类型同名」≠ 兼容**；时序、所有权、取消与迟到消息均为产品契约。

## 1. 传输与分帧

| 项 | 约定 |
|---|---|
| 传输 | Unix Domain Socket |
| 分帧 | **4 字节大端长度** + UTF-8 JSON body（与现 `server.py` 一致） |
| 单消息上限 | 实现须设上限（建议与现实现对齐或文档化，例如数 MB）；超限 → 关闭连接 + error |
| 无效 JSON | 返回 `error` 或关闭；不得半处理 job |
| 断连 | 客户端关 socket = 取消语义可触发；worker 须回收 ffmpeg/线程 |

## 2. 协议版本与 capability（6.5 起强制）

为避免静默 fallback，握手扩展（可兼容旧客户端）：

```json
// client → worker
{"type":"hello","client":"sublift-mac","protocol_version":1}

// worker → client
{
  "type":"bye",
  "protocol_version":1,
  "runtime":"cpp"|"python",
  "engines":["vision","mock","paddle"],
  "capabilities":["path_mode","frame_mode","push_entry","cancel"]
}
```

- 客户端只展示 / 选择 **capability 中的 engines**。
- 请求不可用引擎 → **明确失败**（`done(ok=false)`），禁止静默换成另一引擎。
- `start_job.engine` 与 worker 进程绑定引擎不一致 → 失败（沿用 ADR-0019）。

> 若 6.5 首版需兼容未升级的 Swift：未识别字段忽略；但 C++ worker 仍应发送 capability。

## 3. 消息类型（业务）

与现协议对齐（字段名双端一致）：

| 方向 | type | 角色 |
|---|---|---|
| C→W | `start_job` | 启动任务（含 `video_path` / region / profile / engine / fps…） |
| C→W | `frame` | legacy frame mode JPEG 帧 |
| C→W | `finalize` | 帧流结束 |
| C→W | `cancel_job` | 取消 |
| W→C | `progress` | `pct` + `stage` |
| W→C | `push_entry` | 增量字幕 |
| W→C | `entries` | 成功主响应（批量/最终列表） |
| W→C | `log` | 可忽略 |
| W→C | `done` | `ok` true/false；**失败以 false 为准** |
| W→C | `error` | 协议/连接级错误 |
| 双向 | `hello` / `bye` | 握手 |

## 4. 时序语义（Swift `requestStreaming` 依赖）

Path mode 成功路径（逻辑序）：

```text
start_job
  → (progress | push_entry | log)* 
  → entries          // 成功主响应
  → (可选 done ok=true，客户端可忽略)
```

失败路径：

```text
start_job → … → done(ok=false) 和/或 error
```

**客户端既有逻辑（必须保持）：**

1. `progress` / `log` **可先于**最终结果到达；流式读取时跳过 log、回调 progress。
2. `push_entry` 可先到；**不**代替最终成功判定。
3. 成功以解码到 **`entries`（或约定的主响应类型）** 为准。
4. `done(ok=false)` / `error` → 抛 `serverError`；`done(ok=true)` 在等 `entries` 时可忽略。
5. 连接关闭 → `connectionClosed`（含用户取消关 socket）。

C++ worker **必须**可被现有 `PipelineClient` 驱动（理想情况仅改可执行路径）。

## 5. Path mode / Frame mode 状态机

```text
Idle
  --start_job(path)--> PathRunning
  --start_job(no path)--> FrameAccepting
PathRunning
  --progress/push--> PathRunning
  --entries/done--> Idle
  --cancel/error--> Cleanup --> Idle
FrameAccepting
  --frame*--> FrameAccepting
  --finalize--> Finalizing --> Idle
  --cancel--> Cleanup --> Idle
```

- **同一连接同一时刻一个 job**（与现 bridge 一致）。
- job 进行中再次 `start_job` → 明确错误，不交错。

## 6. 所有权与并发

| 对象 | 规则 |
|---|---|
| Pipeline instance | **单线程**消费；OCR 不并行 |
| ffmpeg | worker 拥有子进程；cancel 时 kill + wait |
| 帧缓冲 | 有界；不得无限积压全片帧 |
| 连接 | 一连接一 handler；job 结束可复用连接启动新 job（现有行为） |

## 7. `video_id` 与迟到消息

- 每条 job 相关推送带 `video_id`（若现协议已有则保持）。
- 客户端过滤：**非当前 job 的 push/progress 必须忽略**。
- Worker 在 cancel/完成后 **抑制**迟到 push（best-effort：cancel 后不再写 push；已写入内核缓冲的除外）。
- 新 job 启动后旧 job 消息不得污染 UI（双端：worker 抑制 + client 过滤）。

## 8. Cancel / Finalize / 资源回收

| 动作 | 要求 |
|---|---|
| `cancel_job` | 设置取消标志；终止 ffmpeg；结束 Pipeline；回复可观测完成/关闭写端 |
| 取消时延 | 产品门：≤ **1s** 停止有效工作（与 Phase 4 长视频验收一致） |
| 重启 | 取消后 ≤ **5s** 可 start 同视频；无旧 entries/错误串扰 |
| `finalize` | 仅 frame mode；path mode 由 worker 读完 pipe 后自终 |
| `finally` | 释放 extractor、线程、临时 socket 文件 |

## 9. 错误码与用户文案

| 层级 | 示例 |
|---|---|
| 用户可操作 | 引擎不可用、文件不存在、engine mismatch、模型未下载 |
| 内部 | 断言、未知异常 → 简短 message + log；不暴露绝对路径可配置 |

`done(ok=false).message` / `error.message` 为 UI 展示主文案；保持 UTF-8。

## 10. 双模式与引擎

见 [engine-matrix-and-cutover.md](engine-matrix-and-cutover.md)。  
Worker 实现可拆为两个二进制或一个多引擎二进制，但 **capability 必须诚实**。

## 11. 协议 fixture 测试（06005 设计 / 065 实现）

共享夹具（建议 `tests/fixtures/ipc/` 或 `cpp/tests/ipc/fixtures/`）：

1. 录制 Python worker 一轮 path mode（mock OCR）消息序。
2. C++ worker 回放输入，比较输出消息 **类型序** 与关键字段。
3. Swift 集成测试：仅改 launch path，mock 或真实 UDS。
4. 用例：engine mismatch、cancel 中途、无效 JSON、断连回收。

06005 交付：夹具格式说明 + 最少 1 个「期望消息序」文本夹具（可先只针对 Python 断言，C++ 6.5 接上）。

## 12. 非目标

- 不在本文重定义 JSON 字段表的每一个可选键（以 `protocol.py` + Swift `Messages.swift` 为字段权威，冲突时双端测试锁）。
- 不要求 6.0 实现 worker。
