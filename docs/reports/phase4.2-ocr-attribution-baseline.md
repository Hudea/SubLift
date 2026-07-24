# Phase 4.2 — Vision OCR 内部性能归因基线

> **状态：** feat-043 已完成；本报告是 OCR 优化分流依据，不是产品速度基线
> **日期：** 2026-07-24
> **验证提交：** `abafab2e209a523522d74ef09a30f2264805fa0b`（`git_dirty=false`）
> **协议：** 同机、同负载、Apple Vision、warmup=1 + measured=3、独立进程、median 为主统计
> **原始产物：** `debug/perf_reports/phase4.2_ocr_attribution/`（不入库）

## 1. 结论（先读）

在 canonical ROI 负载上，**OCR parent 几乎全部落在 Apple Vision 的
`performRequests` 请求执行**：

| 内部阶段（summary，三次 measured 的典型占比） | 占 OCR parent |
|---|---:|
| `vision_perform` | **≈ 98.4% – 99.0%** |
| `input_prepare` + `request_setup` | ≈ 0.7% – 1.4% |
| `observation_mapping` | ≈ 0.15% |
| `residual` | ≈ 0.07% – 0.12% |

在 summary 的本次测量中，**`ocr` 仍是 core wall 最大叶子**（median 约 **43%**），高于
`extract_wait`（27%）与 `signature`（14%）。这只用于阶段排序，不是跨模式的产品速度数字。

**下一步优化方向：多源 GT 驱动的代表帧排序与有效 OCR 调用实验。**
不要继续在 PIL→CGImage 桥接、request 构造或 observation 映射上「猜优化」。先补英文、
中英混排、不同字幕位置和不同片源的 GT；随后仅以实验方式评估：能否让已有候选帧更早命中，
从而减少空 OCR retry 或不必要的共识调用。不得直接降低共识阈值或改变默认行为；也不得以
线程/队列/假并发冒充 Vision 加速。

`extract_wait` 若成为后续端到端课题，必须另做 decode/filter/pipe 归因；它不是本 feature 的
OCR 内部结论。

本报告是**归因与决策证据**，不是速度优化验收。

## 2. 固定负载与复现命令

| 项 | 固定值 |
|---|---|
| 视频 / GT | `debug/Zootopia_clip_1080p.mp4`（254.272 s，不入库）+ `benchmark/fixtures/Zootopia_clip_1080p_gt.srt`（87 条） |
| 采样 / OCR | 5fps、Apple Vision、`subtitle_script=cjk`、confidence=0.5 |
| 区域 | source-frame `[0,848,1920,87]`，`frame_output_mode=roi`（1920×87 RGB） |
| 协议 | warmup=1 + measured=3，off / summary / trace 各一轮独立 manifest |
| 环境 | macOS 26.5.2 arm64，Python 3.12.13，ffmpeg 8.1，Vision 可用 |
| Commit | `abafab2` clean |

```bash
uv run --extra vision python scripts/run_benchmark_manifest.py \
  debug/perf_reports/phase4.2_ocr_attribution/manifest_off.json --label manifest
uv run --extra vision python scripts/run_benchmark_manifest.py \
  debug/perf_reports/phase4.2_ocr_attribution/manifest_summary.json --label manifest
uv run --extra vision python scripts/run_benchmark_manifest.py \
  debug/perf_reports/phase4.2_ocr_attribution/manifest_trace.json --label manifest
```

产物目录：

| 模式 | agent JSON |
|---|---|
| off | `.../phase42_off/Zootopia_clip_1080p_phase42_off.agent.json` |
| summary | `.../phase42_summary/Zootopia_clip_1080p_phase42_summary.agent.json` |
| trace | `.../phase42_trace/Zootopia_clip_1080p_phase42_trace.agent.json` + `.perf-segments/runN.jsonl` |

## 3. 硬门结果总表

| 门 | 要求 | 结果 |
|---|---|---|
| 结果等价 | off / summary / trace `detection_hash` 一致 | **PASS** 全部 `b2d35c1e25f156e1` |
| 固定 GT | F1≥95.2%、precision≥98.8%、usable≥85.1%、CER≤6.6%、noise≤2、empty≤1 | **PASS** 全部 measured run |
| 调用数对账 | summary/trace 中 `ocr_breakdown.call_count == stages.ocr.count == throughput.ocr_calls` | **PASS** 各 measured run 均为 **110** |
| parent 内部对账 | components 覆盖度 ≥80%；delta 合理 | **PASS** coverage ≈ **100.0%**，delta ≪ 0.1 ms |
| parent ↔ outer | 内部 parent 与 `stages.ocr` wall 接近 | **PASS** 交叉差约 **10–13 ms / 110 calls**（≈0.1 ms/call 边界开销） |
| 隐私 | trace 无文本 / 绝对路径 / box 像素 | **PASS**（JSONL 键扫描 + 中文/路径扫描 0 命中） |
| summary 扰动 | summary core_wall median / off wall median ≤ **1.05** | **FAIL** → **1.319**；仅限本报告适用边界（见 §5） |
| trace 扰动 | trace core_wall / off wall ≤ **1.10** | **PASS** → **1.026** |

> off 模式无 `PerformanceRecorder`，wall 取 quality 块 `elapsed_seconds×1000`；summary/trace 取 `throughput.core_wall_ms`。同一实现下二者与 elapsed 对齐。off 产物不输出 OCR 调用数，因此本报告不将 110 声称为 off 的直接计数证据；三模式以结果 hash 与固定 GT 等价为准。

质量锚点（三模式、全部 measured 完全一致）：

| 指标 | 实测 | 门 |
|---:|---:|---:|
| timing F1 | 97.67% | ≥95.2% |
| timing precision | 98.82% | ≥98.8% |
| usable subtitle recall | 91.95% | ≥85.1% |
| CER macro | 3.20% | ≤6.6% |
| text.noise / empty | 0 / 0 | ≤2 / ≤1 |
| detected / GT | 85 / 87 | — |

## 4. 端到端 wall 与阶段排序（summary）

### 4.1 core wall（ms）

| 模式 | run1 | run2 | run3 | **median** | min | max |
|---|---:|---:|---:|---:|---:|---:|
| off（elapsed） | 10098.6 | 10754.8 | 9633.9 | **10098.6** | 9633.9 | 10754.8 |
| summary | 13319.2 | 14740.4 | 11996.4 | **13319.2** | 11996.4 | 14740.4 |
| trace | 10361.2 | 10721.9 | 10316.9 | **10361.2** | 10316.9 | 10721.9 |

| 扰动比 | 值 | 门 |
|---|---:|---|
| summary / off | **1.319** | ≤1.05 **FAIL** |
| trace / off | **1.026** | ≤1.10 PASS |

### 4.2 core 叶子阶段（summary，三次 median total）

| 阶段 | median total | 约占 core | 说明 |
|---|---:|---:|---|
| **ocr** | **5734.7 ms** | **43.1%** | 仍为最大单消费者 |
| extract_wait | 3596.4 ms | 27.0% | 解码+filter+RGB+pipe 合成等待 |
| signature | 1918.8 ms | 14.4% | 前景 / dHash |
| changepoint | 1416.4 ms | 10.6% | 状态机 |
| frame_materialize | 324.0 ms | 2.4% | ROI 后已很小 |
| color_convert | 268.0 ms | 2.0% | |
| pipeline_overhead | 120.1 ms | 0.9% | |
| 其它（line_select / consensus / crop / …） | <40 ms | <0.3% | |

`stage_coverage_pct` 三次均约 **99.9998%**，阶段可解释为同次 core 的分解，而非重叠累计。

## 5. OCR 内部归因（summary 三次 measured）

### 5.1 调用与对账

| 项 | run1 | run2 | run3 |
|---|---:|---:|---:|
| call_count / stages.ocr / throughput | 110 | 110 | 110 |
| parent_total_ms | 5765.2 | 5721.4 | 5447.9 |
| stages.ocr.total_ms | 5775.1 | 5734.7 | 5457.8 |
| coverage_pct | 100.0% | 100.0% | 100.0% |
| engine_detail | vision | vision | vision |

### 5.2 五阶段 total_ms 与 P50/P95（每调用）

**run1（示例，结构三次一致）：**

| 阶段 | total_ms | mean_ms | p50_ms | p95_ms | 占 parent |
|---|---:|---:|---:|---:|---:|
| vision_perform | 5707.6 | 51.89 | 45.81 | 103.48 | **99.00%** |
| input_prepare | 27.5 | 0.25 | 0.18 | 0.51 | 0.48% |
| request_setup | 15.0 | 0.14 | 0.07 | 0.14 | 0.26% |
| observation_mapping | 8.4 | 0.08 | 0.06 | 0.14 | 0.15% |
| residual | 6.7 | 0.06 | 0.03 | 0.05 | 0.12% |

三次 run 的 `vision_perform` 占比均在 **98.4%–99.0%**。桥接与 Python 映射合计 **<2%**。

### 5.3 输入几何

全部 110 次调用均为：

```text
1920 × 87 × RGB   count=110
```

与 ROI 输出一致；无几何抖动。

### 5.4 段内决策（三次完全一致）

| 项 | 值 |
|---|---:|
| closed segment decisions（accepted） | 92 |
| rejected | 0 |
| representative_frames_total | 360 |
| actual_ocr_calls_total | 110 |
| early_stop: `single_high_confidence` | **77** |
| early_stop: `two_frame_consensus` | **15** |
| early_stop: exhausted / 其它 | 0 |

解读：段策略已把 360 个代表候选收敛到 **110 次真实 OCR**。其中 **76 段只调用一次**，
15 段各调用两次形成共识，另有 1 段在前三次空结果后第四次成功。92 个接受段若每段至少
调用一次，当前素材上的理论下限为 92 次；最多仅可减少 18/110（16.4%），且那会触及双帧
共识的质量保障。因而下一实验应优先验证代表帧排序能否消除空 retry，而不是直接放宽早停或
共识阈值。

### 5.5 关于 summary 扰动失败

1. trace 更接近 off（1.026），而 summary 为 1.319；这**排除了“trace 写盘必然更慢”这一简单解释**，但不能证明 summary 的差异完全来自 Vision/系统方差。三组只各有三次、且按模式分块运行，无法区分系统状态、Vision 调度和 summary 路径的影响。
2. summary 的三次 wall（12.0–14.7 s）均高于 off 的三次（9.6–10.8 s），所以 summary 不可作为产品默认路径的速度基线，也不可据此计算任何优化前后 wall 收益。
3. 内部 parent↔outer 的交叉差约 0.1 ms/call，且 `vision_perform` 的直接包络持续占约 99%；这足以支持“优化重点不在桥接/映射”的**归因结论**，但不构成 summary 低扰动的证明。
4. 该扰动失败作为性能模式的限制永久记录。若未来需要把 summary 读数用于产品速度承诺，必须另做交错的 off/summary 配对复测；这不是 feat-043 的未完成项。

## 6. Trace 产物抽查

- 段 JSONL（run3）**92** 行，与 accepted 段数一致。
- 字段含 `representative_selection_ms`、`early_stop_reason`、`ocr_call_details`。其中前者为历史字段名，实际记录的是从段内决策开始到完成的**包容性段决策 wall**，包含代表帧收集、OCR、选行与共识；本报告不将其解释为纯代表帧收集成本。
- `ocr_call_details[]` 仅含宽高/mode/五阶段 ms/outcome，**无文本、无 box、无绝对路径**。

## 7. 分流决策（设计 §5）

| 准则 | 阈值 | 本基线 | 动作 |
|---|---|---|---|
| 1. `vision_perform` ≥ OCR parent 70% | ≥70% | **≈99%** | **命中**：先补多源 GT，再实验代表帧排序与有效 OCR 调用数 |
| 2. `input_prepare+request_setup` ≥20% | ≥20% | **≈1%** | 不立项桥接优化 |
| 3. mapping+line_select+consensus ≥20% | ≥20% | **≪1%** | 不立项 Python 映射/选行优化 |
| 4. OCR 非 core 最大项 | — | OCR **仍是**最大项（43%） | 不转向「放弃 OCR」；但第二大 `extract_wait` 可另立非 OCR 课题 |

**正式建议（唯一）：**
下一 feature 先完成 **多源 GT 扩充**，随后以它为固定门评估**代表帧排序与有效 OCR 调用数**。实验优先减少空 retry；只有所有来源的质量门不退，才可考虑减少双帧共识调用。
**不建议** 作为下一刀：Vision 并行、PIL 桥接重写、observation 映射微优化、输入几何默认改动或再次 producer/consumer 重叠。

## 8. 限制

- 单素材（Zootopia 中文固定底栏）；读数 **不泛化** 到英文、竖排、多行、不同字幕位置或不同 codec；且本轮只有一种输入几何，不能据此授权默认缩放/几何改动。
- summary 扰动门本轮未过；它不能作为产品速度基线。归因结论只由直接的内部计时、parent↔outer 对账及 hash/质量等价支撑。
- `extract_wait` 不可解读为「纯解码」。
- 本报告不授权直接改 OCR 算法默认参数。

## 9. 与既有 ROI 基线的关系

[ROI 通路归因基线](../../benchmark/reports/performance-attribution-baseline.md) 已证明：ROI 后 OCR 是最大叶子。
本报告在 **同一 ROI 路径** 上把该叶子拆开，证明成本在 **`performRequests`**，不在 Python 桥接。两条报告结论衔接：Phase 4 做完搬运；Phase 4.2 看清 Vision 本体。
