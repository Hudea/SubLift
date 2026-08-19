# Path-mode 有界重叠设计（Phase 4.1）

> 本文保留 `feat-042` 的目标架构，**不是当前实现说明**。实验实现的结果、质量、队列与
> 取消均正确，但两轮真实 Vision A/B 未达到吞吐保留门，故未合入 main。当前 GUI 默认 path
> mode 仍在一个 worker 中按“抽一帧 → feed → 可能 OCR”的顺序执行。实际证据以
> [phase41.json](../phases/phase41.json) 为准。

## 1. 决策与问题

Phase 4 已让有效固定字幕区域在 ffmpeg `rgb24` stdout 前裁成 ROI。其 clean-commit
A/B 的 ROI median core wall 为 **9,341.7 ms**；其中 `ocr` 约 **51.65%**、
`extract_wait` 约 **15.03%**、`frame_materialize` 约 **2.50%**。现在的单 worker 会在
每次 OCR 时停止推进 ffmpeg stdout，因而 `extract_wait` 与 OCR 没有重叠机会。

Phase 4.1 的目标是隐藏这部分串行等待，而不是减少 OCR 调用量或并行 Vision：

~~~text
producer thread                              consumer thread
FfmpegExtractor.extract() ─ Queue(maxsize=8) ─→ Pipeline.feed(frame)
       │                    FIFO / backpressure       │
       └── owns extractor + generator                  └→ Pipeline.ocr_segment(...)
                                                            → Pipeline.finalize()
~~~

`Pipeline`、其 timeline/dedupe 状态和 Vision OCR 始终只由一个 consumer 使用，保持
帧序、段序和字幕结果与串行实现相同。Queue 不是产品配置项；容量固定为 8，用户不看见
也不能切换“性能模式”。

## 2. 范围与非目标

本设计只覆盖 GUI 默认的 Python **path mode**，以及为它服务的测试和性能报告。

- 允许 extractor producer 与单个 Pipeline consumer 重叠；不丢帧、不无限缓存。
- 保持 ROI 与 full fallback 路由、算法配置、OCR 代表帧选择、打轴和去重语义不变。
- 让进度反映已被 Pipeline 消费的帧，而非只表示已从 ffmpeg 读出。
- 让取消可打断 producer 的 `read`、producer 的满队列等待及 consumer 的 OCR 阻塞。

明确不做：并行 OCR、改变 `ocr_consensus_frames`、OCR 缓存、候选排序/自适应 OCR、CLI
或 legacy Swift frame mode 重构、GUI 开关、混排质量优化与 GT 扩充。这些将另行立项。

## 3. Job-local 所有权与状态机

当前 `BridgeHandler` 的 `_extractor`、`_pipeline` 和 `_cancelled` 是 handler 级可变字段。
双线程实现不得继续把它们作为跨 job 的事实来源；否则取消后立即启动新任务时，旧线程可
读取或终止新 job 的资源。

目标是每个 path-mode job 创建一个 `PathJobSession`：

| 成员 | 所有者 / 约束 |
|---|---|
| `job_id`, `cancel_event`, `frames: Queue[Frame | Sentinel]` | session 独占；所有线程只读取该 session。 |
| `extractor`, producer thread | producer 独占 generator 推进；`cancel()` 可由协调层调用。 |
| `pipeline`, consumer thread | consumer 独占 `feed`、`ocr_segment`、`finalize` 与条目顺序。 |
| message queue / completion state | 协调协程消费；以 job id 过滤旧 session 的消息。 |
| recorder / queue metrics | session 独占，或经过明确的线程安全聚合；不得使用无锁共享 recorder。 |

~~~text
created → running
running ── EOF consumed ──→ finalizing → completed
running ── cancel/error ──→ stopping  → completed(cancelled/error)
~~~

不变量：

1. producer 只生产 FIFO Frame，consumer 是唯一调用 Pipeline 的线程；
2. 正常 EOF 只由 producer 放入，consumer 消费它之后才可 `finalize()`；
3. 取消先设置 `cancel_event`，再终止 extractor，并唤醒队列上的等待者；取消后不再
   发送 `push_entry`；
4. `done` 只能在 producer 和 consumer 都退出、消息已排空且属于当前 job 后发送；
5. 新 job 不能覆盖旧 job 的资源引用；旧 job 的延迟消息不得污染新 UI。

## 4. Queue、内存与进度语义

producer 在 `Queue(maxsize=8)` 满时以短超时循环 `put()`，每轮检查 `cancel_event`。
这会让 ffmpeg stdout 自然回压，而非将完整视频积压进 Python。不得以无界 `msg_q` 代替
frame queue。

8 帧只是一条上限的队列预算，不是完整 RSS 承诺：以 1920×1080 RGB 估算，full fallback
原始像素约 **47.5 MiB**；Phase 4 canonical ROI（1920×87）约 **3.8 MiB**，还未计
PIL、队列对象、OCR 临时内存和 ffmpeg 缓冲。因此必须以长流 RSS 实测验证“不会随视频
时长线性增长”，并分别覆盖 ROI 与 full fallback。

`progress(frame_count, total_frames)` 应在 consumer 成功取得并处理帧后递增。这样一旦 OCR
变慢，UI 显示的是实际打轴进度，而不是已堆积在 FIFO 中的伪完成度。`finalizing` 之后
不得再发送 `processing`。

## 5. 取消、错误与恢复

| 位置 | 触发 | 目标行为 |
|---|---|---|
| ffmpeg read | producer 正在读取 stdout | `extractor.cancel()` 终止子进程；producer 以取消哨兵退出。 |
| 满队列 | producer 阻塞于 `put()` | 超时循环发现 event，停止生产，不等待 consumer 消化全部帧。 |
| OCR | consumer 在 Vision 调用 | event 阻止后续 feed / entry；OCR 返回后迅速退出，协调层有界 join。 |
| producer 异常 | ffmpeg / 解码异常 | error sentinel 传给 consumer；不执行正常 finalize，不遗留线程。 |

取消从点击到 `done(cancelled)` 的目标是 ≤1.0 s；随后在 ≤5.0 s 内启动相同视频，必须有新
processing 进度且没有旧条目、旧错误、残留 ffmpeg 或残留 worker。若平台上的 Vision 调用
无法强制中断，测试仍须量化最坏延迟并确保不会把该调用之后的结果推入已取消 job。

## 6. 并发性能口径

现有 `PerformanceRecorder` 的 `exclusive_span` 以单线程、全局 stage 计数为假设；直接让
producer 和 consumer 共享它会产生数据竞争，并把重叠耗时错误地相加为 coverage。

feat-042 必须使用线程安全的 session-local 观测模型，至少报告：

| lane / 指标 | 语义 | 可否相加 |
|---|---|---|
| `end_to_end_wall` | job 启动到最终完成的真实 elapsed wall | 否；唯一端到端主指标。 |
| `producer_wall` | extractor 从开始到 EOF/error/cancel 的墙钟范围 | 否；可与 consumer 重叠。 |
| `consumer_wall` | 从首帧消费到 finalize/cancel 的墙钟范围 | 否；可与 producer 重叠。 |
| `queue_high_watermark` | 帧 FIFO 历史最大长度，必须 ≤8 | 不适用。 |
| `queue_backpressure_wait_ms` | producer 因 FIFO 满而等待的累计时间 | 仅诊断，不等同于 core coverage。 |
| RSS | 进程峰值 / 长流样本 | 不适用。 |

如果保留串行 Pipeline 的 leaf stage 统计，报告必须标记其为 **consumer 内局部归因**；不得将
producer、consumer 和 leaf stage 相加后声称 100% coverage。报告应明确重叠部分只可由
wall-time 区间或并发时间线解释。

## 7. 结果等价与验收边界

并发只改变调度，不能改变输入顺序或算法。至少需要：

1. 确定性 fake extractor / OCR 测试证明消费时间戳、关闭段顺序、最终 entries 与串行基线
   完全一致，FIFO high-watermark 永不超过 8；
2. canonical fixed GT 的 `detection_hash` 与串行对照完全一致，且每次 measured 均通过
   F1 ≥95.2%、precision ≥98.8%、usable ≥85.1%、CER macro ≤6.6%、noise ≤2、empty ≤1；
3. 同 clean commit、同机、同 manifest、warmup=1 + measured=3 下，重叠 core/end-to-end
   wall median ≤ 串行基线的 **95%**；目标为 ≤**90%**。若不能稳定达到 95%，不保留该并发
   架构；
4. 首条可见时间 ≤串行基线的 105%，且绝对值 ≤10 s；真实 GUI 的进度单调，且由消费端驱动；
5. 三个取消位置均满足 done ≤1 s、restart ≤5 s；ROI 与 full fallback 都通过；
6. 10 分钟以上长流测试显示 queue 有界，RSS 不随处理时长线性增长。

完整测试矩阵、执行顺序与停机规则见 Phase 4.1 计划。
