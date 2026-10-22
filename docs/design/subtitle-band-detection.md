# 字幕区域自适应条带与水印抑制（Phase 21 D01 设计冻结）

> 状态：D01 设计冻结（2026-10-23）。实现见 D02（C++ server）/ D04（macOS）；文本抑制见 D03。
> 本文只冻结算法、阈值与边界，不复述实现过程。

## 1. 问题

带常驻水印条带的硬字幕视频（例：底部半透明条带内滚动“暖暖的猫 / 98Tang.COM”水印）会出现：

1. **自动选区吸附水印**：现有 `RegionDetector`（C++ server）与 macOS `RegionSelectionModel` 都是
   “多帧抽帧 → 单帧打分 → 该帧候选行 Y min/max + 固定 padding + 4.5% 保底 + X 全宽”。
   水印行每帧都在、位置稳定，会被当作候选并入 ROI，预览文本直接出现水印文本。
2. **全片水印条目**：水印是常驻前景，打轴状态机在无对白时段也判“有字幕”，OCR 选行选中水印行，
   2 小时片按 5fps 会把水印文本铺满整片，体感“无限提取”。
3. **单帧算法不稳**：异常单帧（台标、大标题、字幕缺失帧）会拉宽/偏移 band；固定 padding 与
   4.5% 保底不随字号/行数自适应；没有时间维度区分“常驻水印”和“间歇字幕”。

## 2. 算法：多帧居中式自适应条带

输入：K 个采样帧的 OCR 行（frame_index、归一化 x/y/w/h、text、confidence）。K=8（默认）。

### 2.1 行级外观分

对每个行框（行高 h_ratio = h，cy = y + h/2，cx = x + w/2）：

- 居中分：cx∈[0.33,0.67] → 1.0；cx∈[0.25,0.75] → 0.55；否则 0.05；
- 宽度分：w≥0.15 → 1.0；w≥0.08 → 0.55；否则 0.1；
- 纵向分：cy∈[0.55,0.96] → 1.0；cy∈[0.45,0.55) → 0.4；否则 0；
- 置信度分：0.3×clamp(conf)；
- 水印扣分：文本含 URL/域名（http(s)://、www.、`token.tld`，tld 见实现）→ -1.6；
  贴边小框（cx<0.15 或 cx>0.85 且 w<0.20）→ -0.8。

行参与计算的前提：cy∈[0.50,0.985]，h∈[0.8%,11%] 画面高，conf≥0.40；不满足的行不参与锚点/扩散。

### 2.2 跨帧支持与锚点

- 两行几何匹配：|cy1-cy2| ≤ 0.6×h1，行高比 h2/h1∈[0.55,1.7]，|cx1-cx2| ≤ 0.22。
- 支持帧数 = 与锚点候选几何匹配的不同 frame_index 数（含自身）。
- 间歇性因子：支持帧占比 sf=support/K；sf∈[0.25,0.90] → 1.0；sf>0.90（全帧常驻）→ 0.45；
  sf<0.25（单帧孤证）→ 0.6。
- 锚点分 = 外观分 × 间歇性因子。候选须非 URL；**锚点分 ≥ 1.0** 才可成为锚点
  （单帧孤证经过 0.45 折减后通常不达标，因此“playhead 落在只有水印的帧”会 fail-closed；
  真实单帧字幕外观分约 3+，折减后仍达标）。取最高分为锚点。

### 2.3 居中外扩（band 高度自适应）

从锚点出发，反复吸收同时满足以下条件的行，直到不再变化：

- 外观分 ≥ 0.9，非 URL；
- 行高比（相对锚点行高）∈[0.5,1.8]；
- 行中心 x 与锚点中心 x 偏移 ≤ 0.30；
- 与当前 band 的纵向间距 ≤ max(0.012, 0.9×锚点行高)。

最终 band：

- y0 = min(行 y) - pad_y，y1 = max(行 y+h) + pad_y；pad_y = max(0.008, 0.30×锚点行高)；
  clamp 到 [0,1]，最小高度 0.03（防御性）。
- X 默认全宽（0..1），保证不裁掉未采样到的长字幕。
- **侧边水印 X 收窄（可选，默认开启）**：仅当存在“侧边常驻水印族”时启用：
  - 水印族条件：贴边（cx<0.22 或 cx>0.78 且 w<0.35）或 URL，支持帧占比 ≥ 0.9，
    且与字幕行 X 并集几乎无重叠（<10%）；
  - X = 字幕行 X 并集 ± pad_x（pad_x = max(0.015, 0.15×字幕跨度)），再以字幕中心扩到最小宽度 0.60；
  - 若扩宽后与侧边水印 X 重叠，则放弃收窄、回退全宽（宁宽勿窄）。
- preview_text = 吸收行的文本（去重、按 y/x 排序、上限 8 条）；跳过 URL 行以及
  “宽度 <15% 且码点数 <6”的水印碎片，避免把水印文本展示给用户；confidence = 吸收行置信度均值。
- 诊断：anchor_frame_index / anchor_center_y / anchor_line_height / anchor_support_frames /
  included_lines / excluded_watermark_lines / excluded_texts / reason。

### 2.4 失败与回退

- 输入为空、无满足条件的锚点（例如全片只有 URL/台标行）→ `detected=false`，
  reason=`empty_input` / `no_subtitle_anchor`，**不返回水印框**；detect-region 响应新增
  可选 `reason` 字段（成功为空串）供 UI/调试展示，旧客户端忽略即可。
- 单帧模式（playhead 重检，K=1）使用同一算法，锚点即当前帧最佳字幕行；允许 support=1。
- 侧边水印 X 收窄失败 → 回退全宽，不影响 Y band。
- ROI 一旦返回，任务期间固定；自适应只发生在检测阶段。

## 3. 与 D03 的边界

band 几何能排除“与字幕不同 Y 或不同 X”的水印/台标；但**水印与字幕同一 Y 带且横向扫过**
时，矩形 ROI 无法分离，需要 D03 的行级（URL/域名、混排等）与条目级（全片重复覆盖）抑制兜底。
两者组合才是完整方案。

## 4. 验证口径（D02）

- 片源：`debug/watermark-repro/watermark_band_32s.mp4`（静态左水印）、
  `scrolling_watermark_32s.mp4`（滚动水印）、`debug/band-test/band_1080p.mp4`、
  `debug/Zootopia_clip_1080p.mp4`。
- 检测 A/B：旧/新 `POST /api/video/detect-region` 的 box、preview_text、detected/error。
- 单测：`cpp/tests/subtitle_band_test.cpp`（8 类场景）+ `cpp/tests/fixtures/subtitle_band/`
  录制 OCR 行 fixture 的黄金期望（y/h 容差 0.02）。
- 兼容：detect-region 响应字段不变；默认参数配置化；无字幕视频 fail-closed 行为可解释。

## 5. 阈值来源与风险

- 阈值来自 4 片源原型（debug/band-prototype/）标定；均为常量，集中在
  `SubtitleBandConfig`，后续可按片源调整。
- 风险：极长字幕行未被任何采样帧覆盖时，X 收窄可能裁边——因此默认只在“侧边常驻水印族”
  且“与字幕 X 无重叠”时收窄，并保留 0.60 最小宽度与回退全宽策略。
- 风险：字幕与对白几乎无空档（support 接近 K）时，锚点依赖外观分与 URL/贴边扣分；
  若整片只有居中常驻横幅，可能被判为字幕——此时 fail-closed 不触发，但 D03 会兜底文本层。

## 6. D03 水印文本抑制（实现快照）

当水印与字幕处于同一 Y 带、矩形 ROI 无法分离时，由文本层兜底：

**行级（cpp/src/core/line_select.cpp）**

- `is_watermark_text()`：URL/域名（复用 D02 的 `contains_watermark_domain`，单一口径）
  + 短 CJK 混排 URL token（`98Ta`/`98Tang`）+ 部分/未知 TLD 碎片（`98Tang.cor`/`98Tang.`）。
- `score_line()` 对命中行把 total 封顶 0.02；`select_line()` 拒绝该行。
- 兜底：粘连 salvage（`第三句台词内容Tang.COM` → `第三句台词内容`；中间粘连
  `暖暖的猫 98Tang.cor第四句测试文本` → `第四句测试文本`）；同帧存在 URL 行时，按字号/位置
  抑制被切开的短纯 CJK 碎片。
- 已知边界：同帧无 URL 证据的裸昵称纯 CJK 行，行级无法与对白区分，交条目级。

**条目级（cpp/src/core/dedupe.cpp + pipeline.cpp）**

- `suppress_watermark_entries(entries, video_duration_ms)` 纯函数：候选子集 = 短文本（≤12 码点）
  或 URLish；union-find 近似族（复用 containment/subsequence + 有界 edit similarity，m≤4096
  fail-safe）；族内条目 ≥5、覆盖时长 ≥8% 视频、时间跨度 ≥60% 视频时整族移除。
- 安全阀：族必须带水印信号（≥1 条 URL/混排文本，或 ≥2 种 OCR 变体），避免循环片/重复对白
  （如同句重复 7 次恰好覆盖 13%、跨度 77%）被整族误删。
- `Pipeline::finalize()` 在 `merge_entries` 前应用；环境开关 `SUBLIFT_WATERMARK_FILTER=0`
  只关闭条目级（默认开启）；不改 Config 结构与协议。

## 7. D04 macOS 同口径（实现快照）

- 新增 `SubtitleBandDetector.swift`（C++ D02 的逐条移植）+ `RegionSelectionAggregator`：
  detect 从“最佳单帧”改为保留全部采样帧候选（含无文字帧）→ band 决策；mergedRegion 由 band
  生成；自动选中排除 band 外/水印行；无锚点 fail-closed。
- `RegionSelectionModel.regionBoxForIPC` 在 band 自动模式下保留真实 x/width，使侧边水印 X
  收窄真正进入 start_job region_box；手动选择保持旧的 RegionMerger 全宽契约。
- RegionInspector 的合并摘要显示实际 X 范围（收窄时），不再固定“X 全宽”。

