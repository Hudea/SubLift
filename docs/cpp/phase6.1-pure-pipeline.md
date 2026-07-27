# Phase 6.1 — Pure pipeline 算法 parity

> 子阶段编码：`S = 1` → feature 前缀 `feat-061xx`  
> 任务跟踪：`docs/phases/phase6.json`  
> 总览：[phase6-overview.md](phase6-overview.md)  
> 契约：[parity-contract.md](parity-contract.md) · [architecture.md](architecture.md)  
> **门槛：** `feat-06001`–`feat-06005` 全部 `done`（已满足）

## 1. 目标

将 Python **纯算法** pipeline 模块迁入 `sublift_core`，以**冻结 Oracle + golden 中间量**做逐模块 parity。  
**本子阶段结束时，产品默认路径仍为 Python**（不 feed/ocr_segment 编排、不 Worker cutover）。

迁移顺序（硬依赖，不可跳序验收）：

```text
signature  →  changepoint  →  timeline  →  dedupe  →  line_select
   06101          06102          06103       06104        06105
```

| 模块 | Python 源 | 性质 | Golden 主产物 |
|---|---|---|---|
| signature | `src/sublift/pipeline/signature.py` | 无状态纯函数 | `signature.jsonl` |
| changepoint | `…/changepoint.py` | 有状态状态机 | `events.jsonl` |
| timeline | `…/timeline.py` | 有状态事件→段 | `segments.jsonl` |
| dedupe | `…/dedupe.py` | 无状态 4-pass | `entries.jsonl`（去重后） |
| line_select | `…/line_select.py` | 无状态纯函数 | `ocr_decisions.jsonl` / 行选结果 |

`pipeline/core.py`（`feed` / `ocr_segment` / `finalize`）属于 **6.2**，不在 6.1 范围。

## 2. 做 / 不做

### 做

1. 在 `sublift_core` 实现上表五模块的 C++ candidate（API 与 Python 行为对齐）。
2. 扩展 `scripts/parity/` dump 脚本 + `benchmark/parity/goldens/` 小 fixture。
3. Catch2 `[parity]` 测试：Python dump → 磁盘 golden → C++ load/compare（**测试时不调 Python**）。
4. 复现 parity-contract §5.4 历史行为（色域、时间戳公式等），**不**顺手修算法。
5. 必要时为 signature/SSIM 打开 OpenCV 链接（实现细节；**公共 API 仍不以 `cv::Mat` 为边界**）。

### 不做

- 不切换 CLI/GUI 默认 runtime。
- 不实现 `Pipeline` 流式编排、ffmpeg extractor、Vision、Worker IPC。
- 不实现原生 Paddle；不接真实 OCR 引擎做 6.1 门禁。
- 不上 libav；不改 Swift。
- 不删除 Python 参考实现。

## 3. 任务一览

| ID | 名称 | 验收一句话 |
|---|---|---|
| **feat-06101** | Signature parity | `compute_signature`（含 dHash / fg_ratio / 灰度路径）golden L0/L1 绿 |
| **feat-06102** | Changepoint parity | `ChangePointDetector` 事件流 `events.jsonl` L0 绿 |
| **feat-06103** | Timeline parity | `TimelineBuilder` 段边界 `segments.jsonl` L0 绿 |
| **feat-06104** | Dedupe parity | `merge_entries` 4-pass `entries.jsonl` L0 绿 |
| **feat-06105** | Line select parity | `select_line` / `consensus_text` / cleanup 等 pure 结果 L0/L1 绿 |

依赖：

```text
feat-06004 (parity harness)
    └── feat-06101 signature
            └── feat-06102 changepoint   # 消费 FrameSignature；SSIM 在 signature 侧
                    └── feat-06103 timeline
            └── feat-06104 dedupe        # 仅依赖 models/SubtitleEntry；可与 06102 并行
            └── feat-06105 line_select   # 仅依赖 OcrLine/profile；可与 06102 并行
```

**推荐执行顺序（一次只做一个）：** 06101 → 06102 → 06103 → 06104 → 06105。  
06104 / 06105 在 06101 完成后技术上可并行，但按 `AGENTS.md` 仍应串行开干。

---

## 4. 任务详述

### feat-06101 — Signature parity

**Python oracle：** `compute_signature`、`compute_dhash`、`hamming_distance`、`compute_ssim`、`compute_foreground_ssim`。

| 必须 | 说明 |
|---|---|
| C++ 类型 | `FrameSignature { int64_t timestamp_ms; double foreground_ratio; uint64_t dhash; }`（或与 golden 可互转的等价表示） |
| 入口 | `compute_signature(ImageView, timestamp_ms, SignatureConfig)`；输入布局对齐 fixture（见 §5） |
| 灰度 | 与 Python `_to_gray` 一致：`RGB2GRAY` 语义路径；若 fixture 注入「BGR 当 RGB」怪癖，**按怪癖复现** |
| Dump | `scripts/parity/dump_signature.py` → `benchmark/parity/goldens/signature/…` |
| Golden 行 | `timestamp_ms`, `fg_ratio`（或 `foreground_ratio`，字段名与 schema 锁定）, `dhash`；可选 SSIM 夹具单独 jsonl |
| 比较 | L0 exact：`timestamp_ms`、`dhash`、hamming；L1 epsilon：`fg_ratio` / SSIM（parity-contract §4） |
| 单测 | 单元级小图 + `[parity][signature]` 对照 frozen golden |
| Oracle | envelope 含 `oracle_commit`、`input_asset_sha256`、`golden_schema_version` |

**不做：** 状态机、Pipeline 喂帧、OpenCV 出现在 public header。

**验收：** dump `--check`（若提供）或 golden 再生一致；`ctest` 含 signature parity；Python `./init.sh` 仍绿。

---

### feat-06102 — Changepoint parity

**Python oracle：** `ChangePointDetector.process` / `reset`；事件 `StateEvent`（`IN` / `OUT` / `CHANGE` + 时间戳 / `prev_end_ms`）。

| 必须 | 说明 |
|---|---|
| 输入 | 预计算的 `FrameSignature` 序列 + 可选 crop（SSIM/patrol 路径需要像素） |
| 输出 golden | `events.jsonl`：每事件 `event_type`, `timestamp_ms`, `prev_end_ms` |
| 比较 | L0 exact 全字段（枚举名稳定映射） |
| 配置 | 使用冻结 `ChangePointConfig` 默认 + 至少 1 条覆盖迟滞 / CHANGE 的 fixture |
| Trace | 6.1 **不要求** C++ 复刻 `TraceRecorder` 诊断旁路；parity 只锁事件流 |

**不做：** Timeline 构建、OCR。

---

### feat-06103 — Timeline parity

**Python oracle：** `TimelineBuilder.consume` / `finalize_open_segment` / `build`。

| 必须 | 说明 |
|---|---|
| 输入 | frozen 或 live 的 `StateEvent` 序列（可用 changepoint golden 作输入） |
| 输出 | `segments.jsonl`：`start_ms`, `end_ms`（null 语义与 Python 一致；finalize 后应闭合） |
| 比较 | L0 exact |

**不做：** 代表帧选取、OCR 调度（属 core Pipeline / 6.2）。

---

### feat-06104 — Dedupe parity

**Python oracle：** `merge_entries`（4-pass：merge → optional empty filter → short filter → merge）。

| 必须 | 说明 |
|---|---|
| 输入 | `SubtitleEntry` 列表 JSON（手工 / 合成 fixture 即可） |
| 输出 | 去重后 `entries.jsonl` |
| 参数 | `merge_gap_ms`, `min_duration_ms`, `drop_empty_text` 与 Config 默认对齐，并覆盖 `drop_empty_text` true/false |
| 比较 | L0：`start_ms`/`end_ms`/text（NFC）/`confidence` exact（Mock 路径） |

**不做：** 与真实 OCR 流水线联调。

---

### feat-06105 — Line select parity

**Python oracle：** `score_line`、`select_line`、`normalize_ocr_text`、`cleanup_subtitle_text`、`consensus_text`、`should_accept_text`、`edit_distance` / `is_similar` 等 pure API（以单测与 golden 覆盖的公开函数为准）。

| 必须 | 说明 |
|---|---|
| 输入 | 固定 `OcrLine` + `SubtitleProfile` JSON fixture（无 Vision） |
| 输出 | 选中行索引/文本、共识 `ConsensusResult`、accept 布尔等 |
| 比较 | text NFC exact；分数 float 按 §4 epsilon；索引 exact |
| Unicode | CJK/Latin 边界与粘连清理规则与 Python 一致（regex 语义） |

**不做：** 真实 OCR；`Pipeline._ocr_segment_with_line_select` 整段编排（6.2）。

---

## 5. Fixture 与色域约定

### 5.1 输入资产

优先**小合成图 / 固定 raw ROI 帧**（可检入），避免大视频依赖：

```text
benchmark/parity/fixtures/signature/   # png 或 .rgb + meta
benchmark/parity/goldens/signature/
benchmark/parity/goldens/events/
benchmark/parity/goldens/segments/
benchmark/parity/goldens/entries/
benchmark/parity/goldens/line_select/
```

大文件策略沿用 parity-contract：manifest 进库，体积过大可 git-lfs / 本地 cache。

### 5.2 色域怪癖（必须文档化到每个 signature fixture）

Python Pipeline 历史上可能出现 **RGB→BGR 后再按 `COLOR_RGB2GRAY` 处理** 的路径。  
6.1 规则：

1. 每个 fixture 的 metadata 声明像素语义：`rgb24` | `bgr24_as_rgb_gray_quirk` | `gray8`。
2. Dump 与 C++ candidate **同一语义** 喂入。
3. **禁止**在 6.1 中「修正」为正确 BGR2GRAY 作为默认行为。

### 5.3 时间戳

`timestamp_ms = int(frame_index / fps * 1000)`（非容器 PTS）。  
纯函数模块测试可直接给定 `timestamp_ms`，不必经过 extractor。

---

## 6. 工程落点（建议路径）

```text
cpp/include/sublift/
  signature.hpp
  changepoint.hpp
  timeline.hpp
  dedupe.hpp
  line_select.hpp
cpp/src/core/
  signature.cpp
  changepoint.cpp
  timeline.cpp
  dedupe.cpp
  line_select.cpp
cpp/tests/
  signature_test.cpp          # 单元
  parity/signature_parity_test.cpp
  …
scripts/parity/
  dump_signature.py
  dump_changepoint.py
  dump_timeline.py
  dump_dedupe.py
  dump_line_select.py
```

- 全部进 **`sublift_core`**（无 ObjC）。
- OpenCV：若启用，仅 `.cpp` 私有依赖；`ImageView` / 自有 buffer 为边界类型。
- JSON 解析 golden 继续放在 `sublift_test_support`（与 config parity 一致）。

---

## 7. 6.1 完成定义

- [ ] `feat-06101`–`feat-06105` 全部 `done` + evidence 写入 `phase6.json`
- [ ] 五类 golden 可复现；`oracle_commit` 策略遵守 parity-contract
- [ ] `ctest` 全绿（含 parity）；`./init.sh`（Python 侧）全绿
- [ ] 产品默认仍为 Python
- [ ] `feature-list.json` → `phase6.pure-pipeline` = `done`，`covers` 填齐
- [x] 下一刀准备：`feat-06201` — 设计见 [phase6.2-pipeline.md](phase6.2-pipeline.md)

## 8. 风险

| 风险 | 缓解 |
|---|---|
| OpenCV 自适应阈值与 Python 像素级差 1 | 小图 golden + 锁定插值/`block_size` 奇数规则；必要时 schema epsilon 评审后 bump |
| SSIM 浮点平台差 | L1 epsilon；禁止字符串比 float |
| 跳过中间量只比 SRT | 本子阶段不产出 SRT 门禁 |
| 顺手「修」色域 | §5.2 + code review |
| 范围膨胀进 core Pipeline | 明确 6.2 边界 |

## 9. 与 6.0 / 6.2 的接口

| 已有（6.0） | 本阶段消费 |
|---|---|
| `ImageBuffer` / `ImageView` | signature / SSIM crop |
| `Config` / `SignatureConfig` / `ChangePointConfig` | 默认与字段对齐 |
| parity harness + config golden | 扩展 dump/compare 模式 |
| models（`SubtitleEntry`、`OcrLine`、box 类型） | dedupe / line_select |

| 6.2 将消费 | 本阶段交付 |
|---|---|
| C++ `Pipeline::feed` 等 | 已 parity 的 detector/timeline/dedupe/line_select 积木 |

---

## 10. 下一实现刀

**`feat-06102` Changepoint parity** — 分支建议 `feat/cpp-6.1-changepoint`；提交前缀 `feat(phase6.1):`。
