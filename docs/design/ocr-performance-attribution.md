# OCR 内部性能归因设计

> 本文定义 `feat-043` 的测量契约，不是速度优化实现说明。任务状态与真实证据以
> [phase4.2.json](../phases/phase4.2.json) 为准，执行顺序与验收见
> [Phase 4.2 计划](../plans/phase4.2-ocr-performance-attribution.md)。

## 1. 目的与边界

Phase 4 ROI 已消除大部分全帧图像搬运；Phase 4.1 的 producer/consumer 实验也证明在单
消费者 Vision 主导时，没有足够稳定的端到端吞吐收益。因此 `feat-043` 只回答：每一次 OCR
以及每一个字幕段的时间具体花在哪里，哪一项值得作为下一项独立优化？

本 feature 不改变 `OcrEngine.recognize(image)` Protocol，不改识别语言、选行、共识、OCR
次数上限、打轴、ROI、CLI/GUI 调度，也不增加用户开关。默认 `off` 路径不创建 recorder；
性能数据只面向 benchmark/开发者。

## 2. 计时树与不双计规则

```text
core wall / coverage leaf
└─ ocr: Pipeline 调用 OcrEngine.recognize() 的完整 wall（只在这里计入 core coverage）
   ├─ input_prepare: PIL RGB 转换、bytes、NSData/CFData、CGImage
   ├─ request_setup: handler 与 VNRecognizeTextRequest 配置
   ├─ vision_perform: handler.performRequests_error_
   ├─ observation_mapping: observations → OcrLine / OcrResult
   └─ residual: Python 调用边界、autorelease pool 收尾和未单列成本

segment total（trace/段级，不参与 core coverage）
├─ representative_selection
├─ n × ocr call（最多 Config.ocr_consensus_frames，当前默认 4）
├─ line_select / cleanup / consensus
└─ acceptance decision + early_stop_reason
```

`ocr` 是既有 `stages.ocr` coverage leaf。内部五项不得再加入 `_COVERAGE_LEAF_STAGES` 或
`stages` 汇总；它们写入独立 `ocr_breakdown`，并满足：

```text
ocr parent total ≈ input_prepare + request_setup + vision_perform
                 + observation_mapping + residual
```

`residual` 是有定义的剩余项，不隐藏误差。每次调用以单调纳秒时钟包络；空结果或异常也要
输出完整耗时与 `outcome=empty|error`，不能因失败调用破坏对账。

## 3. 数据契约

### 3.1 summary（有界）

`.agent.json.performance.ocr_breakdown` 至少包括：

| 字段 | 含义 |
|---|---|
| `call_count` | 与 `stages.ocr.count` 一致的 OCR 调用数 |
| `call_total` / 五个内部阶段 | 每项 `count/total/mean/max/p50/p95`；固定样本上限 |
| `input_geometry` | `(width,height,mode)` 的有界计数或 bucket，不含图像/路径 |
| `segment_decisions` | 代表帧数、实际调用数、早停原因、接受/拒绝的计数 |
| `accounting` | parent、components、residual、delta 和 coverage；明确内部项不计 core coverage |
| `engine_detail` | `vision` 或 `opaque`；非 Vision 引擎必须诚实降级 |

summary 不写原始文本、OCR box、图像、视频绝对路径或每次调用列表。

### 3.2 trace（逐段但有界）

每条 `.perf-segments/*.jsonl` 记录一个段；新增字段：

```json
{
  "representative_selection_ms": 0.0,
  "representative_frames": 3,
  "ocr_calls": 2,
  "early_stop_reason": "two_frame_consensus",
  "ocr_call_details": [{
    "input_width": 1920,
    "input_height": 87,
    "input_mode": "RGB",
    "input_prepare_ms": 0.0,
    "request_setup_ms": 0.0,
    "vision_perform_ms": 0.0,
    "observation_mapping_ms": 0.0,
    "residual_ms": 0.0,
    "total_ms": 0.0,
    "outcome": "success"
  }]
}
```

允许的 `early_stop_reason` 为 `single_high_confidence`、`two_frame_consensus`、
`representative_frames_exhausted`、`no_valid_sample`、`no_region`、`legacy_path`。没有提前
结束时必须显式写 `representative_frames_exhausted`。`ocr_call_details` 数量不超过该段的
实际调用数，且不超过 `ocr_consensus_frames`；禁止写文本、候选内容、图像 bytes、box 或绝对路径。

## 4. 接入方式

`PerformanceRecorder` 仍由 benchmark 创建并由 Pipeline 持有。Vision 通过可选、窄的计时
observer 接收当前调用的 sink；`OcrEngine` Protocol 不增参数。Mock/Paddle 等未实现 observer
的引擎自动记录为 `engine_detail=opaque`，其 `residual` 等于该次 outer OCR wall。

Pipeline 只负责 parent、当前段有界 collector、代表帧/早停/acceptance 的记录与异常收尾。
Vision 只在 `vision.py` 接触 PyObjC；`autorelease_pool` 必须继续包住整个调用，计时不得改变
对象生命周期或 `VNRecognizeTextRequest` 设置。

## 5. 读数与决策

收口报告必须先给出 P50/P95 和总占比，后给出一项下一步建议：

| 主导证据 | 可立项方向（仍需独立计划） |
|---|---|
| `vision_perform` 持续占 OCR parent ≥70% | 研究有效 OCR 调用数或输入几何；不能以线程/队列优化冒充 Vision 加速 |
| `input_prepare + request_setup` ≥20% | 评估 PIL→CGImage 传输、请求构造/复用的安全优化 |
| `observation_mapping + line_select + consensus` ≥20% | 优化 Python 映射、选择或聚类，并以混排 GT 防回归 |
| OCR 不是 core 最大项 | 转向 signature/changepoint/pipeline 归因，不在 OCR 上继续猜测 |

阈值只用于筛选候选，不是自动批准代码变更。单一 Zootopia GT 仅用于性能与质量回归，不能把
其读数推广为英文、混排或不同字幕位置的优化结论。
