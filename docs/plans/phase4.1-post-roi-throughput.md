# phase4.1-post-roi-throughput 设计与执行计划

> 本文件是 Phase 4.1 的设计源头。任务状态与实际验证证据以
> [phase4.1.json](../phases/phase4.1.json) 为准；项目级功能块以
> [feature-list.json](../../feature-list.json) 为准。
>
> **状态：已立项，尚未开始运行代码。** 下文是目标与验收，不得把它当作现有
> path-mode 行为说明。

## 1. 主题与目标

Phase 4.1 主题固定为：

> **ROI 后的可重叠流式吞吐**：在保持 ROI、打轴、OCR 和字幕结果不变的前提下，让
> ffmpeg 抽帧 producer 与单消费者 Pipeline/OCR 重叠运行，以有界队列代替当前的串行
> “读取一帧后才处理一帧”。

Phase 4 已消除了大部分全帧 RGB 搬运；本阶段不再试图“只解码字幕区域”，也不改变 OCR
策略。它解决的是：当前一个 OCR 调用会暂停下一次 stdout 读取，原可重叠的 extractor
等待被串行化。

成功不只意味着 wall time 变小。必须同时证明：

1. 帧序、最终字幕与固定 GT 质量不变；
2. 队列始终有界，内存不随视频时长线性增长；
3. 进度表示已消费/已打轴的帧；
4. 取消、错误、立即重启不会串 job 或留下 ffmpeg / 线程；
5. 性能报告在并发下仍有真实、不可误加的口径。

## 2. 立项依据与可达到的上限

Phase 4 canonical ROI A/B（clean commit `1b4612b`、Vision、1080p、5fps、warmup=1 +
measured=3）结果：

| 项 | ROI median | core wall 占比 |
|---|---:|---:|
| core wall | 9,341.7 ms | 100% |
| OCR | 4,825.5 ms | 51.65% |
| signature | 1,493.0 ms | 15.98% |
| extract_wait | 1,404.0 ms | 15.03% |
| changepoint | 1,116.5 ms | 11.95% |
| frame_materialize | 233.2 ms | 2.50% |

理想调度下，`extract_wait` 和部分 `frame_materialize` 可与 consumer 工作重叠，理论上最多
隐藏约 17–18% core wall；真实收益会受到 Queue 反压、GIL、Vision 时机和启动/收尾开销
限制。因此验收采用双层阈值：**≤95% 是保留架构的最低门，≤90% 是目标**，而不是承诺按
ROI 面积或 stage 份额线性加速。

现有 OCR 段内早停已有效：92 个段中 76 段只 OCR 一帧、15 段两帧、1 段四帧，共 110 次
调用。因此不把“再降 `ocr_consensus_frames`”混入本阶段；那是独立的质量/算法取舍。

## 3. 范围与明确不做

### 3.1 本阶段范围

- GUI 默认 Python path mode 的 bounded producer/consumer；
- `PathJobSession` 的 job-local 资源、消息与取消生命周期；
- consumer 驱动的进度、增量条目和 finalizing 状态；
- 并发安全的性能/队列观测与串行 A/B；
- deterministic、真实 ffmpeg、固定 GT、取消/重启和长流验收。

### 3.2 不做

- 不并行 Vision/OCR，不改变 Pipeline 的单消费者顺序；
- 不改变 ROI/full fallback、采样 fps、OCR 配置、行选择、共识、SSIM、打轴或去重算法；
- 不改 CLI 或 legacy Swift frame mode，不加 GUI 选项；
- 不做 OCR 缓存、候选排序、自适应 OCR 或 `ocr_consensus_frames` 收缩；
- 不把英文/中英混排/不同位置的 GT 扩充混入性能阶段；
- 不以 Mock 长流替代真实 path-mode 长流验收。

## 4. 目标架构

完整所有权、sentinel 与性能口径见 [path-mode-overlap.md](../design/path-mode-overlap.md)。

~~~text
BridgeHandler (async coordinator, current session only)
    │ start_job
    ▼
PathJobSession(job_id, cancel_event, Queue(maxsize=8), message queue)
    ├── producer: FfmpegExtractor.extract() ── Frame / EOF / error ─┐
    │                                                                ▼
    └── consumer: Pipeline.feed() → Pipeline.ocr_segment() → finalize()
                              │
                              └── entry / consumed-progress / result → coordinator → GUI
~~~

核心约束：

- producer 独占 extractor 和 generator；consumer 独占 Pipeline 和 Vision；
- Queue FIFO 且 `maxsize=8`，满时反压，不丢帧、不无界积压；
- EOF 只能在被 consumer 消费后触发 `finalize()`；异常/取消有显式 sentinel；
- consumer 成功消费帧后才推 processing 进度；`finalizing` 后禁止 processing；
- 每条异步消息绑定 job id；旧 session 在新任务启动后不得再更新 UI；
- 完成消息只在两个 worker 都退出后送出。

## 5. feat-042 拆分与交付顺序

`feat-042` 是本 Phase 唯一可独立验收的功能；下列内容只作为其内部子任务，不拆成多个
Phase feature。

| 顺序 | 子任务 | 交付与停止条件 |
|---:|---|---|
| 042a | Job-local session | 用 session 取代跨 job 的 handler 级资源事实；先以 deterministic test 证明旧任务不能取消/污染新任务。未完成前不加线程。 |
| 042b | 有界帧队列 | producer/consumer、EOF/error sentinel、满队列反压；证明帧序/entries 与串行一致，high-watermark ≤8。 |
| 042c | 进度与取消 | consumer 进度、三取消点、entry 抑制、join 和重启；任一点超时或有残留即停止性能调优。 |
| 042d | 并发性能口径 | session-local / thread-safe lanes、queue 指标和报告 schema；禁止用重叠 stage 求和计算 coverage。 |
| 042e | 全量验收 | ROI/full fallback、真实 ffmpeg path mode、clean-commit A/B、固定 GT 和长流 RSS；所有硬门通过后才收口。 |

## 6. 验收条件

### 6.1 结果与质量（硬门）

| 类别 | 必须满足 |
|---|---|
| 帧 / 条目等价 | fake extractor/OCR 与真实 path-mode 对照均证明时间戳序列、关闭段顺序、`entries` 顺序和文本完全一致。 |
| 固定 GT | 同 clean commit 的重叠实现与串行对照 `detection_hash` 完全一致；每次 measured 通过 F1 ≥95.2%、precision ≥98.8%、usable ≥85.1%、CER macro ≤6.6%、noise ≤2、empty ≤1。 |
| 队列边界 | Queue 容量固定为 8；报告与测试均证明 `queue_high_watermark ≤ 8`；不允许丢帧或无限缓存。 |
| 进度语义 | progress 单调，只按 consumer 已处理帧递增；`finalizing` 后不再出现 processing。 |

### 6.2 吞吐与首条（硬门 + 目标）

比较只允许在同一 clean commit、同机、同 canonical ROI manifest、Vision/cjk、warmup=1、
measured=3 独立进程下进行。以 median 为主统计量，并归档 min/max、环境、hash 与质量。

| 指标 | 最低通过 | 目标 |
|---|---:|---:|
| `end_to_end_wall` / 串行 baseline | ≤95% | ≤90% |
| 首条可见时间 / 串行 baseline | ≤105% | 更快或持平 |
| 首条绝对时间 | ≤10 s | — |
| 质量 / hash | 100% 等价 | 100% 等价 |

如果 throughput 未稳定达到 ≤95%，即使功能可用也不把并发架构作为默认实现保留；应记录数据并
回到串行路径，而非为得到数字而混入算法或像素格式变更。

### 6.3 取消、重启和资源（硬门）

| 场景 | 必须满足 |
|---|---|
| ffmpeg read、满队列等待、OCR 阻塞 | 从 cancel 到 `done(cancelled)` 均 ≤1.0 s；无后续 entry / processing。 |
| 立即重启 | 取消后 ≤5.0 s 启动相同视频；有新 processing，且无旧条目、旧错误、旧 done、残留 ffmpeg 或 worker。 |
| ROI / full fallback | 两条路径都通过取消/重启和结果回归；不能只以 ROI 掩盖 full fallback 的内存或同步问题。 |
| 长流内存 | ≥10 分钟真实 path-mode 素材上，队列深度有界；RSS 不随视频时长线性增长。记录 ROI 和 full fallback 的适用样本/限制。 |

### 6.4 可观测性与自动验证（硬门）

1. 报告至少含 `end_to_end_wall`、producer/consumer wall、`queue_high_watermark`、
   `queue_backpressure_wait_ms`、RSS、队列容量、输出模式、环境和质量门；
2. producer/consumer lanes 可重叠，禁止相加为 coverage；保留的 Pipeline leaf stage 明确
   标为 consumer 内归因；
3. 覆盖 deterministic queue、error/EOF、三取消点、重启隔离、progress/finalizing、ROI/full
   fallback 和报告 schema 的自动测试；
4. 收口提交实际运行 `uv run ruff check .`、`uv run mypy src tests`、`uv run pytest`、
   `./init.sh` 和 `cd apps/macos && swift test`，并将真实命令/结果写入 `phase4.1.json`。

## 7. 完成定义与后续分流

Phase 4.1 仅在 `feat-042` 已完成、上述硬门全部有实际证据、文档同步且仓库可通过标准启动
路径时标记 done。当前 `feat-042` 是 `not-started`；本计划本身不是实现或验收证据。

完成后再根据数据选择下一阶段：若 OCR 仍为主导，才独立评估 OCR 策略；若产品质量风险更
重要，则优先扩充英文、中英混排、不同字幕位置和不同片源的 GT。两者都不能借本阶段的性能
报告直接宣称完成。
