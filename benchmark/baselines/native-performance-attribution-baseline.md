# C++ Native 阶段归因基线（Paddle / bottom-crop）

> **状态：** Phase 16 D01（S0）产出的当前默认归因锚
> **验证树：** `opt/native-perf-acceleration` 分支工作树（基线 commit `6b787b2` + Phase 16
> S0 插桩，测量时未提交）；`git_dirty=true` — 数字仅对本机该工作树有效
> **协议：** 同机、同负载、同版本；warmup=1 + measured=3 独立进程，median 为主统计；
> 计时插桩由 `SUBLIFT_STAGE_TIMING=1` 启用，与计时关闭运行的输出 SRT sha256 完全一致

## 目的与范围

本报告回答：C++ Native + Paddle 产品路径在固定负载上的端到端成本分布。它是
[`docs/design/performance-acceleration.md`](../../docs/design/performance-acceleration.md) 第一批
（P0）优化的收益对照系 — 后续每个措施的前后基准都以本报告为 "前值"。

它**不是** Python/Vision 时代基线
（[`performance-attribution-baseline.md`](performance-attribution-baseline.md)）的延续：
运行时、OCR 引擎、解码并发结构都不同，两边的绝对值**不可直接比较**；可比较的是
"阶段占比"这一类结构信息。

## 固定负载

| 项 | 固定值 |
|---|---|
| 视频 / GT | Zootopia 1920×1080、254.272 s；87 条固定 GT（视频不入库，取自 `debug/`） |
| 采样与 OCR | 5fps、Paddle（PP-OCRv6-small，ONNX Runtime 1.28.0，CPU provider） |
| 文字系统 | `subtitle_script=cjk` |
| 区域 | 无 region_box → bottom-crop 检测器，ROI 输出 1920×324（1271 帧） |
| 构建 | macOS arm64、Release（`build/cpp-rel`，Homebrew OpenCV 4.14 / ORT 1.28.0） |
| 插桩 | `SUBLIFT_STAGE_TIMING=1`；阶段计时默认关闭，产品路径零成本 |
| 结果等价 | 计时开启/关闭输出 SRT sha256 均为 `56b1b08acdf2770a…`；81 条字幕 |

## 阶段树与归因（3 次 measured 的 median）

覆盖率 99.9498%（顶层叶子合计 / core_wall）。阶段树（顶层叶子标 `*`）：

```text
extract_wait *        ffmpeg setup + spawn + teardown
frame_fetch *         每帧提取器内部（管道等待）
  frame_materialize   帧缓冲分配（O3 后为零填充豁免）
consume_call *       消费者调用（含帧缓冲释放）
  consume             feed + ocr_segment + IPC + 段事件释放
    signature         前景比例 + dHash
    changepoint       状态机（含 SSIM patrol）
    color_convert     RGB→BGR quirk 转换
    pipeline_overhead feed 编排剩余（排他）
    ocr               ocr_segment
      paddle.global_preprocess / crop
      paddle.det_preprocess / det_infer / det_postprocess
      paddle.cls_preprocess / cls_infer
      paddle.rec_preprocess / rec_infer
finalize *            管道 finalize（去重合并）
```

core_wall median **122,114.0 ms**；job_wall median 122,502.0 ms（含 plan/probe 与引擎创建）。

| 阶段 | calls | total_ms | % of core_wall | 解释 |
|---|---:|---:|---:|---|
| `consume_call` | 1271 | 119,531.0 | 97.89% | 消费者调用总wall（含帧释放） |
| `ocr` | 186 | 110,080.0 | 90.15% | `ocr_segment` 总 wall；绝对主导项 |
| `paddle.det_infer` | 292 | 103,003.0 | 84.35% | Det 推理（占 ocr 的 93.6%） |
| `paddle.rec_infer` | 190 | 4,848.4 | 3.97% | Rec 推理 |
| `paddle.det_preprocess` | 292 | 1,039.2 | 0.85% | Det 标量前处理 |
| `signature` | 1271 | 5,283.2 | 4.33% | 前景比例 + dHash |
| `changepoint` | 1271 | 3,577.3 | 2.93% | 状态机 + SSIM patrol |
| `frame_fetch` | 1272 | 2,154.9 | 1.76% | 管道等待（`extract_wait` 仅 0.2ms） |
| `frame_materialize` | 1272 | 96.6 | 0.08% | 帧缓冲分配（当时含零填充） |
| `color_convert` | 1271 | 473.7 | 0.39% | RGB→BGR 逐字节交换 |
| `finalize` | 1 | 366.2 | 0.30% | 去重合并 |
| `pipeline_overhead` | 1271 | 435.3 | 0.36% | feed 编排剩余 |

### 与设计文档假设的差异（读数结论）

1. **OCR 占比与假设一致且更极端**：90.15%（Vision 时代为 51.65%），其中
   `det_infer` 单项占 core wall 84.35% — 唯一的结构性上限，与 Phase 4.1 负向证据
   （不得以线程/队列优化冒充 OCR 加速）互相印证。
2. **`extract_wait` 从 15.03% 塌缩到 ~0%**：C++ Native 把解码/滤镜/RGB 转换放进
   ffmpeg 子进程，与消费者并行；父进程只承担管道等待。这是与 Python 基线的**结构差异**，
   不是优化收益。
3. **大图像搬运已不是主要矛盾**：`frame_materialize` 0.08%、`paddle.*_preprocess`
   合计 ~1.0%。设计文档中 O3/O4 的收益预估基于 Debug 期数字，Release 实测显著更小
   （O3 实测见下）。
4. **patrol 占比小于预估**：`changepoint` 2.93%（Python 时代 11.95%）。

## O1–O4 前后对比（同协议，Phase 16 D02 证据）

同一固定负载、同一协议（warmup=1 + measured=3，median）：

| 阶段 | 前（S0 only） | 后（S0 + O1–O4） | 变化 |
|---|---:|---:|---:|
| core_wall_ms | 122,114.0 | 122,515.0 | +0.33%（噪声内） |
| `frame_materialize` | 96.6 | 0.5 | **−99.5%**（O3 去零填充） |
| `changepoint` | 3,577.3 | 3,168.2 | −11.4%（O1 掩码缓存 + O2 缓冲复用） |
| `color_convert` | 473.7 | 469.1 | −1.0%（O3 BGR 缓冲去零填充） |
| `paddle.det_preprocess` | 1,039.2 | 1,032.8 | −0.6%（O4 去 clone，噪声内） |
| `ocr` | 110,080.0 | 110,946.0 | +0.79%（噪声内） |
| coverage_pct | 99.9498 | 99.9532 | — |
| 输出 SRT sha256 | `56b1b08acdf2770a…` | `56b1b08acdf2770a…` | **逐位一致** |

**结论**：O1–O4 全部数值等价（golden hash 与三来源 SRT SHA 不变，CTest 253/253、
ASan/UBSan 无发现）；绝对收益集中在 `frame_materialize`（−96.1ms）与 patrol
（−409ms），合计约 core wall 的 0.4% — 与 Release 实测的阶段分布一致（搬运类成本
本就很低）。OCR（84% det 推理）不受 P0 措施影响，其杠杆在 P1/P2
（O14/O16/GPU）与既有冻结门。

## 独立复核：纯 HEAD vs 工作区（D02 数值等价硬门）

上表比较的是「HEAD + S0 插桩」与「HEAD + S0 + O1–O4」，两边都开着计时。为排除插桩本身的影响，
另做了一次**纯 HEAD vs 当前工作区**的独立 A/B（计时关闭）：

| 项 | 值 |
|---|---|
| 基线 | 独立 `git worktree` 于 `6b787b2`（`/tmp/sublift-baseline`，`git status` 干净） |
| 优化 | 当前工作区（O1–O8） |
| 负载 | `debug/Zootopia_clip_1080p.mp4`、`--engine paddle --fps 5 --script cjk` |
| 协议 | Release（`build/cpp-rel`）、无 `SUBLIFT_STAGE_TIMING`、交替执行、各 3 次 |

| 运行 | 基线 wall (s) | 优化 wall (s) | 输出 SRT sha256 |
|---|---:|---:|---|
| 1 | 140.04 | 136.21 | `56b1b08acdf2770a…` |
| 2 | 137.15 | 137.14 | `56b1b08acdf2770a…` |
| 3 | 137.00 | 122.21 | `56b1b08acdf2770a…` |
| median | **137.15** | **136.21** | 一致 |

6 个 SRT 全部 81 条字幕、`cmp` 字节相同，sha256 均为 `56b1b08acdf2770a…`（与本报告固定负载一节
及 D01 记录一致）——O1–O4 在真实视频上逐位等价。**端到端 wall 差异约 0.7%，落在运行噪声内**
（基线极差 3.0s、优化极差 15.0s），不作为加速承诺；这与「90% 成本在 OCR 推理、P0 措施只触及
非 OCR 开销」的读数结论一致。

产物：`debug/ab-phase16/baseline-run{1,2,3}.srt`、`debug/ab-phase16/current-run{1,2,3}.srt`
（`debug/` 不入库）。
### 智能选区（ROI）路径独立复核

同一 A/B 方法，但走 Server 的选区路径：`POST /api/video/detect-region`（engine=paddle）取
固定 box（x=0.0, y=0.7732, w=1.0, h=0.0999，采样 30.51s，confidence 0.99993），再以同一
`region_box` 提交 `POST /api/jobs`（fps 5、script cjk）：

| 运行 | 最终状态 | SRT 行数 | SRT sha256 |
|---|---|---:|---|
| 基线（6b787b2） | completed | 348 | `d6b51602c7a01673…` |
| 优化（工作区） | completed | 348 | `d6b51602c7a01673…` |

两侧 `detect-region` JSON 完全相同，导出 SRT `cmp` 字节相同。该 ROI 输出（348 行）与默认
bottom-crop 输出（324 行）几何不同，说明选区路径确实被执行。产物：
`debug/ab-phase16/{baseline,current}-roi.srt`、`debug/ab-phase16/{baseline,current}-roi-detect-region.json`。
## 复现与更新规则

```bash
# 构建 Release（含 Paddle + Vision + OpenCV）
cmake -S cpp -B build/cpp-rel -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DSUBLIFT_REQUIRE_OPENCV=ON -DSUBLIFT_ENABLE_VISION=ON -DSUBLIFT_ENABLE_PADDLE=ON \
  -DONNXRuntime_INCLUDE_DIR=/opt/homebrew/include/onnxruntime \
  -DONNXRuntime_LIBRARY=/opt/homebrew/lib/libonnxruntime.dylib
cmake --build build/cpp-rel

# 归因测量（warmup=1 + measured=3，median）
uv run python scripts/measure_native_attribution.py \
  --video debug/Zootopia_clip_1080p.mp4 --engine paddle --fps 5 --script cjk \
  --label native_paddle_bottomcrop_5fps
```

只有同一 clean commit、同机、相同负载与构建配置，warmup=1 + measured=3，且计时开关的
输出 SRT hash 一致时，才可更新本报告。覆盖 `coverage_pct` < 99.9% 或 hash 不一致的
运行不构成有效基线。诊断产物（JSON/Markdown）落在 `debug/benchmark/`（不入库）。

> 与 Python/Vision 基线的比较边界：本报告测量的是 Native CLI 产品路径（ffmpeg 子进程
> 并行解码 + Paddle ORT CPU 推理）；旧基线是 Python 进程内 Vision OCR。两边的
> `extract_wait`、`ocr` 绝对值不可比，只有"单消费者最大项是 OCR"这一定性结论一致。
