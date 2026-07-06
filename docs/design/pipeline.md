# 打轴与编排设计

> `src/sublift/pipeline/` — 帧签名、变化点检测、时间轴构建、去重合并、端到端编排。

## 模块职责

| 子模块 | 文件 | 职责 |
|---|---|---|
| 帧签名 | `signature.py` | 计算双信号（前景占比 + dHash），描述单帧字幕带特征 |
| 变化点检测 | `changepoint.py` | 有状态状态机，消费帧签名，产出 IN/OUT/CHANGE 事件 |
| 时间轴构建 | `timeline.py` | 消费事件流，构建 `TimelineSegment` 列表 |
| 去重合并 | `dedupe.py` | 纯函数，合并连续相同段 + 过滤过短段 |
| 端到端编排 | `core.py` | `Pipeline` 类，串联上述四组件 + extractor/detector/ocr |

## 数据流

```
video
  └─[extractor]→ Frame(t, image)
      └─[detector, 首帧一次性]→ Region(box)
          └─[crop]→ 字幕带图像
              └─[signature]→ FrameSignature(fg_ratio, dhash)
                  └─[changepoint]→ StateEvent(IN/OUT/CHANGE) | None
                      └─[timeline]→ TimelineSegment(start_ms, end_ms)
                          └─[ocr, 每段一次]→ text
                              └─[dedupe]→ list[SubtitleEntry]
```

OCR 后置：timeline 全部构建后，每段只调一次 OCR（段首代表帧），而非逐帧 OCR。

## 帧签名双信号

`compute_signature(image, timestamp_ms, config) -> FrameSignature`

两信号独立，状态机分别使用：

| 信号 | 计算方式 | 检测目标 | 原理 |
|---|---|---|---|
| A: 前景占比 | 灰度化 → `cv2.adaptiveThreshold`（高斯加权，`THRESH_BINARY_INV`）→ 2×2 形态学开运算去噪 → 白像素占比 | 字幕存在/消失边界 | 字幕笔画在二值化后是前景，占比反映字幕覆盖程度 |
| B: dHash | 对二值化图像缩放到 `(hash_size+1) × hash_size` → 相邻像素亮度梯度比较 → 64bit 整数 | 字幕内容变化 | 对整体亮度漂移免疫，对结构变化敏感 |

`_compute_block_size` 根据字幕带高度 × `block_size_ratio` 计算自适应阈值块大小，确保为奇数且 ≥ 3。

SSIM 二级验证（`compute_ssim`）：numpy 回退实现，不依赖 scikit-image。接口就位，默认 `enable_ssim_verify=False`。

## 三态状态机

`ChangePointDetector` — 有状态，逐帧调用 `process(signature, crop)`。

### 状态图

```
┌───────┐  fg_ratio ≥ threshold    ┌───────┐  dHash 距离 > threshold
│ EMPTY │◀────────────────────────▶│STABLE │◀───────────────────────┐
└───────┘  连续 N 帧确认迁移        └───────┘  候选→稳定确认→迁移      │
   ▲          (迟滞)                    │                          │
   │  fg_ratio < threshold              │  dHash 距离 > threshold   │
   │  连续 N 帧确认迁移 (迟滞)          ▼  候选→稳定确认→迁移         │
   │                            ┌──────────┐                       │
   └────────────────────────────│STABLE'   │───────────────────────┘
                                └──────────┘
```

### 三个事件

| 事件 | 触发条件 | 状态迁移 |
|---|---|---|
| `IN` | EMPTY 状态下连续 N 帧前景占比 ≥ 阈值 | EMPTY → STABLE |
| `OUT` | STABLE/STABLE' 状态下连续 N 帧前景占比 < 阈值 | → EMPTY |
| `CHANGE` | 与锚帧 dHash 距离 > 阈值 + 新内容稳定确认 | STABLE → STABLE' |

### 关键机制

- **迟滞确认**（`hysteresis_frames=2`）：连续 N 帧满足条件才迁移状态，抗单帧闪烁。
- **时间戳回溯**：事件 `timestamp_ms` 用信号**首次出现**的帧时间戳（`_first_presence_ms`/`_first_absence_ms`/`_change_candidate_ms`），非迟滞确认帧，保证打轴精度。
- **候选/稳定确认双阶段**：dHash 触发变化后不立即确认，记录候选时间戳，等新内容与上一帧 dHash 距离 ≤ 阈值（`_is_new_content_stable`）才确认，抗字幕过渡抖动。
- **SSIM 二级验证**（可选）：dHash 触发候选后，用 SSIM 比较当前帧与锚帧，相似度 > 阈值则否决候选（`_is_ssim_vetoed`），过滤 dHash 误报。默认关闭。

## 时间轴构建

`TimelineBuilder` — 有状态，消费 `StateEvent` 流。

| 事件 | 动作 |
|---|---|
| `IN` | 开新段（`start_ms = event.timestamp_ms`） |
| `OUT` | 关闭当前段（`end_ms = event.timestamp_ms`） |
| `CHANGE` | 关闭当前段（`end_ms = event.prev_end_ms`）+ 开新段（`start_ms = event.timestamp_ms`） |

`finalize_open_segment(last_timestamp_ms)`：视频结束时字幕仍在，用最后一帧时间戳关闭末尾段。

## 去重 3-pass

`merge_entries(entries, merge_gap_ms, min_duration_ms) -> list[SubtitleEntry]`

纯函数，不改变输入。3-pass 顺序关键：

| Pass | 操作 | 逻辑 |
|---|---|---|
| 1 | 合并 | 相邻相同文本（归一化后相等）+ gap ≤ `merge_gap_ms` → 合并，保留首段文本 |
| 2 | 过滤 | `duration < min_duration_ms` → 丢弃 |
| 3 | 合并 | Pass 2 删段后原本被隔开的相同段变相邻 → 再合并一次 |

**归一化**（`_normalize`）：移除所有空白字符。中文字幕中空白通常是 OCR 噪声，移除后比较更稳健。

Pass 1 在前的原因：两个短相同段合并后可能变合法（duration ≥ 阈值），先合并再过滤能保留这种情况。

## Pipeline 编排

`Pipeline(extractor, detector, ocr, config)` — 依赖注入，`run(video_path) -> list[SubtitleEntry]`。

### 串联顺序

1. `extractor.extract(video_path)` → 帧迭代器
2. 首帧 `detector.detect(frame)` → Region（一次性，非逐帧）
3. 逐帧：`crop` → `compute_signature` → `changepoint.process`
4. 事件驱动 `timeline_builder.consume`
5. `finalize_open_segment` → `TimelineSegment` 列表
6. 每段取 `anchor_frames[start_ms]` 代表帧 → `ocr.recognize` → `SubtitleEntry`
7. `merge_entries` 最终清理

### 内存控制

只缓存事件触发帧（`anchor_frames: dict[int, Frame]`），不缓存全部帧。OCR 后置每段调一次，而非逐帧。

### confidence 过滤

OCR 返回 `confidence < config.confidence_threshold` 时，text 置空（保留段时间轴，text 为空）。

## 配置参数

### SignatureConfig

| 参数 | 默认值 | 说明 |
|---|---|---|
| `block_size_ratio` | 0.08 | 自适应二值化块大小比例（相对字幕带高度） |
| `adaptive_c` | 12 | 自适应阈值常数 C，从局部均值减去 |
| `hash_size` | 8 | dHash 尺寸（8×8=64bit） |

### ChangePointConfig

| 参数 | 默认值 | 说明 |
|---|---|---|
| `presence_threshold` | 0.01 | 前景占比阈值，> 此值判定有字幕 |
| `hysteresis_frames` | 2 | 迟滞确认帧数 |
| `change_threshold` | 10 | dHash 汉明距离阈值，> 此值判定内容变化 |
| `enable_ssim_verify` | False | SSIM 两级验证开关（接口就位，MVP 关闭） |
| `ssim_threshold` | 0.95 | SSIM 相似度阈值 |
| `ssim_window_size` | 7 | SSIM 滑动窗口大小（奇数） |
| `enable_ssim_patrol` | False | SSIM 巡逻开关（feat-031b，推荐开启） |
| `ssim_patrol_interval` | 3 | SSIM 巡逻间隔（帧数） |
| `ssim_patrol_threshold` | 0.92 | SSIM 巡逻阈值，前景 SSIM < 此值视为结构变化 |
| `ssim_patrol_use_mask` | True | 巡逻时优先比较二值化前景 mask（屏蔽背景变化） |

## SSIM 巡逻机制（feat-031b）

SSIM patrol 解决 dHash 对中文短句/相似布局判别力不足的问题。在 STABLE
状态下，dHash 未触发时（`distance <= change_threshold`），周期性比较
当前帧与锚帧的前景结构：

1. 每 `ssim_patrol_interval` 帧执行一次巡逻
2. 对锚帧与当前帧分别做自适应二值化，在二值图上算 SSIM（`use_mask=True`）
3. 当 SSIM < `ssim_patrol_threshold` 时，产生 CHANGE 候选
4. 候选经 `_is_new_content_stable` 稳定确认后触发 CHANGE 事件

patrol 与 dHash 候选共享 `_change_candidate_ms`，但 `trigger_reason` 区分。
patrol 与 `enable_ssim_verify` 方向相反（verify 防 FP，patrol 防 FN），可
同时启用。

**推荐配置**：`enable_ssim_patrol=True, interval=3, threshold=0.92`。
在 Zootopia clip 上 F1 从 65.0% 提升到 80.6%（+15.6pp），recall 从 60.9%
提升到 90.8%，precision 不下降。默认关闭以保持向后兼容，建议生产环境
显式开启。

## 已知限制

- **dHash 对中文判别力不足**：9×8 降采样丢失汉字笔画高频信息，两句长度相近的中文字幕 dHash 距离可能 < 阈值，导致漏分段。详见 `docs/HURDLES.md`。**已通过 SSIM patrol 部分缓解**（feat-031b），merged_into_neighbor FN 减少约 70%。
- **短字幕漏检**：duration < 1.5s 的短字幕召回率仍偏低（~60%），主要属于 IN/OUT 或采样不足，SSIM patrol 不能解决。详见 `docs/HURDLES.md`。
- **OCR 锚帧过渡画面空文本**：锚帧（IN/CHANGE 事件触发帧）可能落在字幕淡入/切换瞬间，Vision 识别不出文字。详见 `docs/HURDLES.md`。首选待评估方案：锚帧延后 N 帧。
- **字幕区域裁剪过宽**：`bottom_ratio=0.3` 是通用默认值，未针对实际视频校准。裁剪过宽会把画面上方英文标题/新闻栏误纳入，Vision 误识别为字幕，导致 OCR 字符准确率低。详见 `docs/HURDLES.md`。首选待评估方案：调小 bottom_ratio 或加 CLI 区域参数。

端到端实测基线（Zootopia clip, 1080p, 5fps, region_box 精准对齐）：打轴 F1 65.0%（baseline）/ 80.6%（patrol 启用）。
