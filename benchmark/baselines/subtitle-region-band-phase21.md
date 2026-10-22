# Phase 21 基线：水印条带场景（D01）

> 记录时间：2026-10-23。环境：macOS 27.0.1 (26A434)、Xcode 27.0、Apple M4、Debug 构建、
> Paddle PP-OCRv6-small、ORT 1.28.0。所有数字可在本机复现；原始产物在 `debug/phase21/`（gitignored）。

## 1. 片源

| 片源 | 分辨率/时长 | 内容 |
|---|---|---|
| `debug/watermark-repro/watermark_band_32s.mp4` | 1280x720 / 32s | 底部半透明条带：左侧静态水印“暖暖的猫 / 98Tang.COM”（x≈0.02–0.17，y≈0.85–0.92）+ 居中中文字幕 8 句（2.0s 显示 / 0.6s 间隔，y≈0.84–0.91） |
| `debug/watermark-repro/scrolling_watermark_32s.mp4` | 1280x720 / 32s | 同上，水印改为整宽滚动（4px/frame） |
| `debug/band-test/band_1080p.mp4` | 1920x1080 / 60s | 底部居中静态演示字幕（无长驻水印） |
| `debug/Zootopia_clip_1080p.mp4` | 1920x1080 / 254.25s | 真实硬字幕（letterbox），字幕自 26s 起；无长驻水印 |

生成脚本（合成片）：`debug/watermark-repro/gen.py`、`gen_scroll.py`（Pillow + ffmpeg，产物不入库）。

## 2. 现状 detect-region 基线（server，engine=paddle）

命令：`POST /api/video/detect-region`，body `{"video_path":"<abs>","engine":"paddle"}`。

| 片源 | detected | suggested_box (x,y,w,h) | preview_text | sample_time_s | candidates |
|---|---|---|---|---|---|
| watermark_band_32s | true | 0.0, 0.8227, 1.0, 0.1198 | 这是第二句字幕 暖暖的猫 98Tang.COM | 3.84 | 3 |
| scrolling_watermark_32s | true | 0.0, 0.8227, 1.0, 0.1073 | 这是第二句字幕 暖暖的猫 98Tang.COM | 3.84 | 2 |
| band_1080p | true | 0.0, 0.8917, 1.0, 0.1083 | 第一行中文 | 15.00 | 1 |
| Zootopia_clip_1080p | true | 0.0, 0.7732, 1.0, 0.0999 | 好啦，你也演太大了 | 30.51 | 1 |

问题证据：前两行 preview_text 已含水印文本；box 为 X 全宽、Y 覆盖水印行。

## 3. 现状提取基线（CLI，--fps 5 --engine paddle --script cjk）

命令：`build/cpp/bin/sublift_cli extract <video> -o <srt> --fps 5 --engine paddle --script cjk`
（基线二进制已冻结到 `debug/phase21/baseline-build/`）。

| 片源 | 条目数 | 水印条目数 | 说明 |
|---|---|---|---|
| watermark_band_32s（默认 ROI） | 2 | 0 | 静态水印未触发条目，但打轴被拉长（0–8.4s / 8.4–31.8s，漏检 6 句） |
| scrolling_watermark_32s（默认 ROI） | 10 | 3 | “暖暖的猫98Tang.COM / 暖暖的猫98 / 暖暖的猫” |
| scrolling_watermark_32s（detect-region 返回 ROI 0.8227/0.1073） | 15 | 6 | 6 条水印 + 字幕行被污染（“第三句台词内容Tang.CoM”） |

说明：水印条目按“文本含域名/昵称水印或明显非字幕内容”人工判定；D03 以此为 A/B 基线。

## 4. 现状全片基线（无长驻水印，用于回归）

Phase 20 冻结：`debug/Zootopia_clip_1080p.mkv` 全片默认 0.6MP 的 SRT sha256 =
`f77296e3736be0f0…`（完整命令见 `benchmark/baselines/paddle-operator-attribution-phase20.md`）。
D02/D03 改动后必须保持该 hash 不变（D03 规则不得误杀该片）。

## 5. 现状检测耗时

- detect-region（7 采样点，含 Paddle OCR）：约 2.5–4s（本机 Debug；随帧内容波动）。
- 新算法不增加 OCR 帧数上限（仍为现有 7 点），聚类/打分 <10ms。

## 6. 结论

现状单帧合并会把水印行并入 ROI 并写进 preview；带水印 ROI 提取会产出全片水印条目。
D02/D03 的目标与验收见 `docs/phases/phase21.json` 与 `docs/design/subtitle-band-detection.md`。

## 7. D02 自适应条带 A/B（2026-10-23）

实现：`sublift::detect_subtitle_band`（cpp/src/core/subtitle_band.cpp）接入
`RegionDetector`；采样 7 帧全部 OCR（不再单帧早停），跨帧支持度 + 间歇性锚点 + 居中外扩。
检测耗时 2.3–5.0s（含 Paddle OCR，见每行实测）。

命令：`POST /api/video/detect-region`，body `{"video_path":"<abs>","engine":"paddle"}`；
新响应新增 `reason` 字段（detected=false 时可解释，成功为空串，兼容旧客户端）。

| 片源 | 旧 box (x,y,w,h) | 新 box (x,y,w,h) | 旧 preview_text | 新 preview_text |
|---|---|---|---|---|
| watermark_band_32s | 0.0000, 0.8227, 1.0000, 0.1198 | **0.2000, 0.8090, 0.6000, 0.1222** | 这是第二句字幕 暖暖的猫 98Tang.COM | 第四句测试文本 第七句演示文本 …（仅字幕） |
| scrolling_watermark_32s | 0.0000, 0.8227, 1.0000, 0.1073 | 0.0000, 0.8049, 1.0000, 0.1375 | 这是第二句字幕 暖暖的猫 98Tang.COM | 第八句结束测试 …（仅字幕） |
| band_1080p | 0.0000, 0.8917, 1.0000, 0.1083 | 0.0000, 0.8756, 1.0000, 0.1244 | 第一行中文 | 第一行中文 Rec 全链路 跨平台上 最终验收 |
| Zootopia_clip_1080p | 0.0000, 0.7732, 1.0000, 0.0999 | 0.0000, 0.7494, 1.0000, 0.1381 | 好啦，你也演太大了 | (前市长杨咩咩入狱)N PRISON … 好啦，你也演太大了 … |

关键结论：

- 静态左水印片：新框 X 收窄到 0.2000–0.8000（原始水印 x<0.17），preview 不再出现水印文本；
- 滚动水印片：Y band 内仍与字幕重叠（矩形 ROI 无法分离），但预览已过滤水印碎片，残留交由 D03 文本抑制；
- 无长驻水印片：Y band 略增高（多帧行范围 + 行高自适应 padding），无 X 收窄（无侧边水印族）；
- 检测耗时 2.3–5.0s（旧算法早停时约 2.5–4s；新算法固定 7 帧证据，属可接受的一次性检测成本）。

## 8. Fail-closed 行为 A/B（2026-10-23）

| 场景 | 旧 detector | 新 detector |
|---|---|---|
| playhead t=5s（无字幕，仅静态水印） | detected=true, box y=0.8241 h=0.1212, preview “暖暖的猫 98Tang.COM” | **detected=false, reason=no_subtitle_anchor** |
| 全黑无文本视频 | detected=false | detected=false（reason 可解释路径一致） |
| playhead t=14s（有字幕） | detected=true（含字幕帧） | detected=true, box 0.1992,0.8090,0.6000,0.1222, preview “第六句字幕内容” |

D02 单测：`./build/cpp/bin/sublift_tests "[subtitle_band]"` → 16 test cases / 82 assertions
（单行/多行/侧边水印 X 收窄/滚动 URL 水印/台标大框/空输入/仅 URL/playhead 水印帧 fail-closed/
letterbox/4 片源 fixture golden）。

## 6. 结论

现状单帧合并会把水印行并入 ROI 并写进 preview；带水印 ROI 提取会产出全片水印条目。
D02/D03 的目标与验收见 `docs/phases/phase21.json` 与 `docs/design/subtitle-band-detection.md`。

## 9. D02+D03 端到端提取 A/B（2026-10-23）

环境：Release 构建（build/cpp-rel，最终树），Paddle PP-OCRv6-small，`--fps 5 --script cjk`；
基线二进制为冻结的 main 构建（debug/phase21/baseline-build）。

### 9.1 默认 ROI（CLI）

| 片源 | 基线 | 最终树 | 水印条目 | 中文字幕 |
|---|---|---|---|---|
| `scrolling_watermark_32s.mp4` | 10 条 | **7 条** | 3 → **0** | 7 句全部保留 |
| `scrolling_watermark_128s.mp4`（32s loop×4） | 40 条 | **28 条** | 15 → **0** | 28 句 = 7×4，全部保留（含 salvage 回收的“第四句测试文本”） |

128s 输出 sha256 `506b15bb0e5921910aebbb2c661d894d48f05ca9c8eb263645981f60fe1f2dfb`；
基线 `099f10a3998bdbebaf1e9d7562a43758bc886ad2832d69d1656512064fade91c`。

### 9.2 detect-region ROI 提取归因（32s）

| 组合 | 条目 | 水印条目 | 字幕保留 |
|---|---|---|---|
| 旧 detector ROI + 旧 pipeline（scrolling） | 15 | 7 | 8 句，1 句被污染（`第三句台词内容Tang.CoM`） |
| 新 detector ROI + 旧 pipeline（watermark_band，静态左水印） | 8 | 0 | 8 句全部保留（X 收窄排除左侧水印） |
| 旧 detector ROI + 新 pipeline（scrolling） | 8 | 0 | 8 句全部保留（1 句残留 OCR 噪声 `第三包台词内容coM`，非水印） |
| 新 detector ROI + 新 pipeline（watermark_band / scrolling） | 8 | 0 | 8 句全部保留 |

结论：D02 负责“把水印排除出 ROI”（静态侧水印 X 收窄；无锚点 fail-closed）；D03 负责
“ROI 内仍与字幕重叠的水印文本”兜底；两者组合后 32s 片 15 条/7 水印 → 8 条/0 水印，
128s 片 40 条/15 水印 → 28 条/0 水印。

### 9.3 无长驻水印片零影响

| 片源 | 基线 sha256 | 最终树 sha256 | 条目 |
|---|---|---|---|
| `debug/Zootopia_clip_1080p.mp4`（Release） | `f77296e3736be0f09d3752eedb7cc2e5f3f9ceaef0a15eb6936f160d4854fce7` | 同左 | 80 → 80 |
| `debug/Zootopia_clip_1080p.mkv`（Debug/worker 由 D03 队友复核） | `ce856f6a5e700d2eb12342035977a4da56bd982d4da03881f5837108e4538abe` | 同左 | 82 → 82 |

### 9.4 测试

- `[watermark]`：22 cases / 87 assertions；全量 Debug CTest：**319/319 passed**。
- `[subtitle_band]`：16 cases / 82 assertions。
- Swift：XCTest 383 tests / 0 failures；swift-testing 167 tests / 16 suites passed。
- Web Vitest：17 files / 159 tests passed。

