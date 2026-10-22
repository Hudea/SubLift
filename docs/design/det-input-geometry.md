# Det 输入几何：从 min-side 736 到像素预算

> **状态**：实验与实现分支 `exp/det-pixel-budget`（2026-10-21），未改产品默认值，未合入
> `main`。几何策略已收敛为可测试接口（`DetInputGeometry` / `det_input_size`）并由
> 产品配置 `--det-pixel-budget` / Server `det_pixel_budget` 驱动（2026-10-22 起；
> `SUBLIFT_DET_PIXEL_BUDGET` 仅作 adapter 3 参入口的诊断别名）；Phase 18 D04 起默认
> 0.6MP，显式 `--det-pixel-budget 0` 回滚 736。交付与验证见
> [`det-pixel-budget-delivery.md`](det-pixel-budget-delivery.md)。
> **结论先行**：当前 `limit_side_len=736, limit_type=min` 会把任意 bottom-crop 拉到
> 4352×736（3.2MP）；改为"像素预算约 0.7MP"后，固定负载端到端 **快 2.9–3.3×**
> （det 单项约 4.7×）。按项目官方 one-to-one 诊断，5 个素材组合（Zootopia 三档 + 2 个
> fixture；三档同源下采样）上 CER 全部下降、exact 上升；但条目 split / timing precision
> 并非全面改善，不能概括为"质量全面不降"。736 还会让 det 框把长行切碎、丢失左半段或整行。

## 1. 问题

`prepare_det_input` 复刻 RapidOCR 的 min-side 规则：短边不足 736 就整体放大。
1920×324 的 bottom-crop 因此变成 4352×736：

| 源 bottom-crop | 736 规则下的 det 输入 | 像素 |
|---|---|---:|
| 1920×324（1080p） | 4352×736 | 3.20MP |
| 1280×216（720p） | 4352×736 | 3.20MP |
| 854×144（480p）  | 4352×736 | 3.20MP |
| 960×162（540p fixture） | 4352×736 | 3.20MP |

det 成本与像素近似线性（D01 归因：84.35% core_wall；实测 ≈120ms/MP），于是**成本与源分辨率
无关，永远按 3.2MP 付**。同时上采样不增加信息，只引入插值模糊。

## 2. 候选策略

- `min736`（现状）：短边不足 736 → 放大到 736。
- `native`：不放大（短边保持原样）。
- `budget`：目标面积 B，仅在面积 < B 时放大到 B（**不缩小**）。

实验实现：`SUBLIFT_DET_MIN_SIDE`、`SUBLIFT_DET_PIXEL_BUDGET` 两个环境变量；实验期默认
736，Phase 18 D04 起产品默认 0.6MP（显式 0 = 736 回滚）。
（2026-10-22 起产品路径改用 `--det-pixel-budget` / Server `det_pixel_budget`，env 只对 adapter 3 参入口有效。）

## 3. 方法

- 构建：工作树 `build/cpp-rel`（Release，OpenCV 4.14 / ORT 1.28.0 / Paddle）。
- 固定负载：`Zootopia_clip_1080p.mp4`（254s，5fps，cjk，bottom-crop）及其 720p/480p 下采样版；
  `paddle_quality` 的 `mixed_multiline`（cjk）、`latin_motion`（`--script latin`，960×540，GT 各 10 条）。
  **注意**：Zootopia 三档是同源下采样，不是三个独立内容；独立内容源只有 Zootopia、
  mixed_multiline、latin_motion 三个。0.7MP 又是在 480p 扫描中选定的，存在调参集复用。
- 质量（历史口径）：实验期脚本按时间重叠取最大 IoU，**允许多个 GT 复用同一条 det，且不惩罚
  未匹配的 det**；只适合快速筛策略。2026-10-21 复核改用项目官方
  `sublift.benchmark.diagnostics.analyze_entries`：one-to-one、temporal IoU 0.5，同时报告
  CER macro/micro、exact、text coverage、timing recall/precision、FP/split。
- 计时：除 wall 外，用 `SUBLIFT_STAGE_TIMING=1` 复核 OCR / det / rec 的调用数与耗时；
  计时开关前后 SRT sha256 一致。
- 每个配置单次运行；0.7MP 策略在 1080p 上复跑一次验确定性。

## 4. 结果

### 4.1 Zootopia 三档（旧口径快速评估）

下表 matched CER 来自实验期脚本，允许多个 GT 复用同一 det、且不惩罚未匹配 det；
命中数因此偏高（86/87），只用于筛策略，不作为最终质量结论。官方复核见 4.4。

| 源 | 策略 | det 输入 | wall | 条目 | exact | matched CER |
|---|---|---|---:|---:|---:|---:|
| 1080p | 736 | 4352×736 | 120s | 81 | 57 | 25.97% |
| 1080p | native | 1920×320 | **40s** | 81 | 73 | 13.29% |
| 1080p | budget 0.7MP | 2048×352 | 41s | 83 | 73 | **13.15%** |
| 720p | 736 | 4352×736 | 119s | 80 | 62 | 20.58% |
| 720p | native | 1280×224 | 22s | 81 | 73 | 16.40% |
| 720p | budget 0.7MP | 2048×352 | 37s | 82 | 74 | **13.16%** |
| 480p | 736 | 4352×736 | 96s | 80 | 67 | 17.81% |
| 480p | native | 864×128 | 11s | 78 | 73 | 18.18% |
| 480p | budget 0.7MP | 2048×352 | 29s | 80 | 74 | 12.81% |

确定性：1080p / 0.7MP 复跑 SRT sha256 完全相同（`e667beef1110af7b…`）。
基线保真：1080p / 736 输出 sha256 = `56b1b08acdf2770a…`，与产品现状逐位一致。

### 4.2 像素预算扫描（480p 源，旧口径）

| 预算 | wall | exact | matched CER |
|---|---:|---:|---:|
| native（0.12MP） | 11s | 73 | 18.18% |
| 0.4MP | 19s | 77 | **10.71%** |
| 0.7MP | 29s | 74 | 12.81% |
| 1.4MP | 47s | 74 | 12.73% |
| 3.2MP（736） | 96s | 67 | 17.81% |

质量是 U 形：太小（native）与太大（3.2MP）都差，0.4–0.8MP 是宽平台。单样本差异不足以
定精确最优点，0.7MP 是可解释的折中（1080p 恰好略高于原生 0.62MP，低分辨率有足够上采样）。

### 4.3 质量门 fixture（960×540，旧口径）

| fixture | 策略 | wall | exact | matched CER |
|---|---|---:|---:|---:|
| mixed_multiline | 736 | 7s | 2/10 | 55.12% |
| mixed_multiline | 0.7MP | 4s | **9/10** | **7.50%** |
| latin_motion | 736 | 217s | 1/10 | 38.53% |
| latin_motion | 0.7MP | 68s | **10/10** | **0.00%** |

**复核更正（2026-10-21）**：`latin_motion` 必须用 `--script latin` 跑。实验期数字本身可复现
（重跑 736 用 217.21s、0.7MP 用 67.64s，matched CER 38.53% → 0.00%，exact 1/10 → 10/10），
但工作树里遗留的 `q_latin_motion_p*.srt` 后来被 `--script cjk` 覆盖，直接用它们会得到两边
CER 100%、不能作为证据。官方口径下 latin 的 timing precision 仅 22.0% → 39.1%，仍有大量
split/多余条目；0.00% 只是“匹配上的 10 条全对”，不是整体质量。

736 的失败形态可以直接看到（mixed_multiline）：
`SubLift 自动提取 100%` → `自动提取`；`C++ Paddle 原生质量门` → `e 原生质量门`；
多行条目第二行 `Second line mixed` 整行消失。0.7MP 三处全部正确。

### 4.4 官方 one-to-one 复核（2026-10-21）

用 `sublift.benchmark.diagnostics.analyze_entries`（temporal IoU 0.5）对同一批 SRT 重新评分：

| 素材 | 策略 | CER macro | CER micro | exact | text coverage | timing recall | timing precision | FP | split |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 1080p | 736 | 17.27% | 17.88% | 72.6% | 83.9% | 83.9% | 90.1% | 7 | 1 |
| 1080p | 0.7MP | **3.67%** | **2.95%** | **92.0%** | **86.2%** | **86.2%** | 90.4% | **4** | 4 |
| 720p | 736 | 13.18% | 14.32% | 76.6% | 88.5% | 88.5% | 96.2% | 3 | 0 |
| 720p | 0.7MP | **4.83%** | **4.05%** | **92.3%** | **89.7%** | **89.7%** | 95.1% | **1** | 3 |
| 480p | 736 | 8.78% | 8.70% | 84.4% | 88.5% | 88.5% | 96.2% | 2 | 1 |
| 480p | 0.7MP | **3.13%** | **2.54%** | **94.7%** | 87.4% | 87.4% | 95.0% | 4 | 0 |
| mixed_multiline | 736 | 55.12% | 61.15% | 20.0% | 100% | 100% | 100% | 0 | 0 |
| mixed_multiline | 0.7MP | **7.50%** | **10.79%** | **90.0%** | 100% | 100% | 100% | 0 | 0 |
| latin_motion | 736 | 37.62% | 37.07% | 11.1% | 90.0% | 90.0% | 22.0% | 24 | 8 |
| latin_motion | 0.7MP | **0.00%** | **0.00%** | **100%** | 90.0% | 90.0% | **39.1%** | **12** | **2** |

latin 行来自 2026-10-21 的 `--script latin` 重跑（217.21s / 67.64s），不是被 cjk 覆盖的旧工件。

结论：

- **文本质量稳定改善**：5 个素材 CER macro 全部下降（最大 55.12% → 7.50%，1080p
  17.27% → 3.67%），exact 全部上升；这不是旧脚本匹配口径造成的假象。
- **时间轴不是全面改善**：1080p/720p recall 略升，480p recall 87.4% 略低于 88.5%；
  720p/480p timing precision 各降约 1pp；1080p/720p 的 split 从 1/0 增到 4/3。
- **latin 仍差**：precision 22.0% → 39.1% 是改善，但 39.1% 仍然很低；0.7MP 只是把
  “匹配上的 10 条全对”，并没有解决条目级的多框/碎片问题。

### 4.5 成本归因复核（`SUBLIFT_STAGE_TIMING=1`，反向顺序）

先跑 0.7MP、再跑 736，排除“先跑冷、后跑热”的偏向：

| 项 | 736 | 0.7MP | 变化 |
|---|---:|---:|---:|
| wall | 150.42s | 48.34s | **3.11×** |
| core_wall | 149,750ms | 47,687ms | −102,063ms |
| `ocr` calls | 186 | 186 | 0 |
| `paddle.det_infer` calls | 292 | 287 | −5 |
| `paddle.det_infer` total | 127,576ms | 26,477ms | **−101,099ms** |
| `paddle.det_infer` / call | 436.9ms | 92.3ms | **4.74×** |
| `paddle.rec_infer` calls | 190 | 261 | +71 |
| `paddle.rec_infer` total | 5,707ms | 6,759ms | +1,052ms |

det 省下的 101.1s 解释了 core wall 差异的 **99.1%**；OCR segment 调用数完全相同，
Rec 调用反而多了 37%。因此加速不是“少跑 OCR”或换引擎，而是 det 输入像素下降。
计时开关下两次 SRT sha256 仍分别为 `56b1b08a…` / `e667beef…`，与无计时运行一致。
无计时的同负载 A/B 为 120.26s → 41.50s（2.90×）；两种测量方向一致。

## 5. 解释

det 成本 ∝ 输入像素；736 规则让成本恒等于 3.2MP。质量下降与"框破碎"一致：在 5.9:1 的超宽
条带上把短边放大 2–5×，DB 后处理倾向把长行拆成多个 quad，随后整行/半行丢失。0.4–0.8MP
既保留足够笔画，又不至于让框在极端长宽比下碎裂。但 0.7MP 也会在部分素材增加 split/短碎片
（4.4），所以"质量提升"只适用于文本 CER/exact，不适用于所有条目级指标。

## 6. 建议（默认切换未实施，需决策）

1. **默认策略改为像素预算**（建议 0.7MP，或 `min(max(0.6MP, native), 1.0MP)` 的变体），
   替换固定 736。固定负载实测 120.26s → 41.50s（2.90×），官方 CER/exact 显著改善，但
   split/timing precision 有轻微退步。策略与单测已落地；Phase 18 D04 已切换默认 0.6MP，回滚走 `--det-pixel-budget 0`。
2. 这会改变 det 框与 SRT 输出 → **RapidOCR parity / golden hash 必须作为独立决策重冻**，
   不得以优化名义静默合入。
3. 合入前补：真实低分辨率源（非下采样合成）、竖屏/超宽条带、全帧路径与 region 检测路径；
   并用 `paddle_quality` 全门复跑（含 box recall、timing F1 与 CER 门）。
4. 需要排除的替代解释：先用 Python/RapidOCR 参考实现在同一 crop、同一 736 参数下复现，
   确认"框破碎"来自几何而非 C++ DB 后处理移植偏差。
5. 质量结论统一用官方 one-to-one 诊断，不再用实验期"多对一、只算匹配项"的 matched CER
   作为合并依据；旧的 86/87 命中数不可与官方 recall 直接比较。
6. 0.7MP 在 480p 扫描中选定，Zootopia 三档同源；虽然两个独立 fixture 也改善，仍需在
   未参与调参的素材上确认 0.4–0.8MP 平台，不能把 0.7MP 当成普适最优点。

## 7. 复现

当前实现分支为 `exp/det-pixel-budget`，直接在主工作树构建：

```bash
cmake -S cpp -B build/cpp-rel -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DSUBLIFT_REQUIRE_OPENCV=ON -DSUBLIFT_ENABLE_VISION=ON -DSUBLIFT_ENABLE_PADDLE=ON
cmake --build build/cpp-rel -j 8

# 默认 0.6MP、736 回滚与 0.7MP
SUBLIFT_WORKER_PATH=build/cpp-rel/bin/sublift_worker \
  build/cpp-rel/bin/sublift extract debug/Zootopia_clip_1080p.mp4 \
  -o /tmp/a.srt --engine paddle --fps 5 --script cjk --det-pixel-budget 0

SUBLIFT_WORKER_PATH=build/cpp-rel/bin/sublift_worker \
  build/cpp-rel/bin/sublift extract debug/Zootopia_clip_1080p.mp4 \
  -o /tmp/b.srt --engine paddle --fps 5 --script cjk --det-pixel-budget 700000

# latin fixture 必须用 latin，否则会得到 cjk 垃圾结果
SUBLIFT_WORKER_PATH=build/cpp-rel/bin/sublift_worker \
  build/cpp-rel/bin/sublift extract debug/paddle-quality/latin_motion_180s.mp4 \
  -o /tmp/latin-a.srt --engine paddle --fps 5 --script latin --det-pixel-budget 0
SUBLIFT_WORKER_PATH=build/cpp-rel/bin/sublift_worker \
  build/cpp-rel/bin/sublift extract debug/paddle-quality/latin_motion_180s.mp4 \
  -o /tmp/latin-b.srt --engine paddle --fps 5 --script latin --det-pixel-budget 700000

# 官方 one-to-one 质量诊断
PYTHONPATH=src python3 - <<'PY'
from pathlib import Path
from sublift.benchmark.srt import load_srt
from sublift.benchmark.diagnostics import analyze_entries
for det in ('/tmp/a.srt', '/tmp/b.srt'):
    d = analyze_entries(load_srt(Path(det)),
                        load_srt(Path('benchmark/datasets/Zootopia_clip_1080p_gt.srt')),
                        temporal_iou_threshold=0.5)
    print(det, d.metrics.recognition.cer_macro, d.metrics.timing.timing_precision)
PY
```

阶段归因：在命令前加 `SUBLIFT_STAGE_TIMING=1`，stderr 会输出单行 JSON；对比
`ocr`、`paddle.det_infer`、`paddle.rec_infer` 的 `calls` 与 `total_ms`。

## 8. 补充验证（Phase 18 D04）

- 480p（854×480，254.25s）：736 98.94s → 默认 0.6MP 25.71s（**3.85×**）。
- 4K 60s（3840×2160，global 2000 上限，60s）：736 40.85s → 0.6MP 17.03s（**2.40×**）。
- 与 1080p（2.79–3.09×）、720p 下采样（3.74×）、latin（3.17×）、mixed（2.31×）合并，
  主 debug 视频均 ≥2×；full-frame/portrait/ultrawide 短 fixture 因 native 面积不缩小而 <2×。
- 官方 `./scripts/verify-product.sh`：22 项通过，仅 `swift test` 在 DSH 沙箱因
  `sandbox-exec: sandbox_apply: Operation not permitted` 失败；等价
  `swift test --disable-sandbox` 通过 381 XCTest + 143 Swift Testing，其余 CTest/Web/
  CLI/Server 检查均通过。
