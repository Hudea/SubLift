# Worker IPC 契约

本契约适用于 macOS/Native CLI 与 C++ Worker，也约束显式 Python IPC runtime 的兼容面。字段真源是
`cpp/include/sublift/protocol/` 与 Swift `Messages.swift`；双端集成测试锁定实际行为。

## 1. 传输与 framing

| 项 | 契约 |
|---|---|
| 传输 | 本地 Unix Domain Socket |
| framing | 4 字节大端无符号长度 + UTF-8 JSON body |
| 单消息上限 | 64 MiB |
| 非法输入 | 零长度、超限、截断、非法 JSON 或 schema 错误必须 fail-closed |
| 断连 | 取消活动任务并回收 ffmpeg、线程、连接与 socket 文件 |

客户端发送 `bye` 后 Worker 不再回复第二个 `bye`，而是清理任务并关闭连接。

## 2. 握手与 capability

```text
client: hello(client, protocol_version)
worker: bye(protocol_version, runtime, engines, capabilities)
```

`engines` 只列当前进程可实际接受的引擎；请求未知引擎、缺失 capability 或
`start_job.engine` 与进程绑定引擎不一致时必须失败。完整路由规则见
[Runtime 契约](runtime-contract.md)。

## 3. 消息

| 方向 | type | 作用 |
|---|---|---|
| C→W | `hello` / `bye` | 握手与关闭 |
| C→W | `start_job` | 启动 path mode 或 frame mode |
| C→W | `frame` / `finalize` | 兼容 frame mode 输入与结束 |
| C→W | `cancel_job` | 取消当前任务 |
| W→C | `progress` / `push_entry` / `log` | 进度、增量字幕和诊断 |
| W→C | `entries` | 成功任务的最终权威结果 |
| W→C | `done` / `error` | 任务失败、协议错误与完成身份 |

所有 job 消息使用 `video_id` 关联。客户端忽略非当前 job 的迟到推送；Worker 在取消或完成后抑制旧 job 输出。

## 4. 时序

成功路径：

```text
start_job
  → (progress | push_entry | log)*
  → entries
  → done(ok=true)?
```

失败路径：

```text
start_job → (progress | log)* → done(ok=false) 和/或 error
```

- `push_entry` 只用于增量呈现，不构成成功判定。
- `entries` 是编辑与导出的最终结果；客户端用它替换增量预览。
- `done(ok=false)`、`error`、异常 EOF 或 framing 错误均按失败处理。
- 同一连接同一时刻只允许一个 job；活动任务期间再次 `start_job` 必须拒绝。

## 5. 模式与生命周期

```text
Idle ──start_job(video_path)──► PathRunning ──entries/error/cancel──► Idle
Idle ──start_job(no path)─────► FrameAccepting ──finalize──────────► Idle
```

- Path mode 由 Worker 拥有 Extractor、ffmpeg 子进程和 Pipeline。
- Frame mode 只保留为兼容/调试路径，`finalize` 仅用于该模式。
- 单个 Pipeline 和 OCR 实例串行消费；帧缓冲必须有界。
- 取消设置协作标志、终止并 wait ffmpeg、停止后续推送并释放 job 资源。
- 取消后应在 1 秒内停止有效工作，并在 5 秒内允许新任务启动且无旧消息串扰。

## 6. 验证

`tests/fixtures/ipc/` 保存协议序列夹具；C++ protocol/worker 测试覆盖 framing、schema、握手、
engine mismatch、path/frame mode、取消、非法输入和断连；Swift 集成测试验证
`PipelineClient` 可直接驱动目标 Worker。
