# SubLift 独立多模态硬字幕真源（GT）工具集 (`tools/multimodal-gt`)

本目录提供一套与 SubLift 产品运行时完全解耦、可独立运行的**多模态硬字幕绝对真源（Ground Truth）制备与对标工具**。
详细方法论与 `Zootopia_clip_1080p.mp4` 盲测 A/B 实证报告见 [`docs/design/multimodal-gt-methodology.md`](../../docs/design/multimodal-gt-methodology.md)。

---

## 1. 目录结构

- [`build_filmstrips.py`](./build_filmstrips.py)：连续解码抽帧 + 横向 16 分块前景笔画掩码最大差计算 + 渲染带时间码烙印的胶片拼图（支持 `--mode strict-rle | uniform | both`）。仅依赖 `ffmpeg`、`ffprobe`、`numpy`、`Pillow`。
- [`stitch_and_evaluate.py`](./stitch_and_evaluate.py)：将多模态输出的帧号区间 JSON 批次自动缝合转换为标准 `.srt`（按半开区间 `[start_frame * 100ms, (end_frame + 1) * 100ms]` 映射），并支持与参考 GT 做误差评测。

---

## 2. 快速运行命令

### 2.1 生成拼图（默认 `strict-rle` 模式，省 ~76% 页数且边界无损）

```bash
# 对指定视频生成严格折叠拼图（Scheme B，默认）
.venv/bin/python tools/multimodal-gt/build_filmstrips.py \
  --video debug/Zootopia_clip_1080p.mp4 \
  --out-dir debug/gt_experiment \
  --mode strict-rle

# 同时生成 Scheme A（全量无损 77 页）与 Scheme B（严格折叠 18 页）用于 A/B 对比
.venv/bin/python tools/multimodal-gt/build_filmstrips.py \
  --video debug/Zootopia_clip_1080p.mp4 \
  --out-dir debug/gt_experiment \
  --mode both
```

### 2.2 缝合多模态 JSON 批次并导出 `.srt`

```bash
# 将多模态识别出的 JSON 批次合并输出为 SRT，并可选与参考 GT 对标
.venv/bin/python tools/multimodal-gt/stitch_and_evaluate.py \
  --inputs batch1.json batch2.json \
  --out-srt debug/gt_experiment/output_gt.srt \
  --gt-srt benchmark/datasets/Zootopia_clip_1080p_gt.srt

# 一键复现 Zootopia_clip_1080p.mp4 的方案 A vs 方案 B 盲测对标结果
.venv/bin/python tools/multimodal-gt/stitch_and_evaluate.py --reproduce-zootopia-ab
```

---

## 3. 多模态转录标准 Prompt 与 JSON Schema

将生成的 `schemeB_page_01.png .. schemeB_page_NN.png` 分批传给强多模态模型时，使用以下 Prompt 模板：

```text
请按顺序阅读这些双列字幕胶片拼图（每张图先读左列从上到下，再读右列从上到下）。
每行左侧标签为 10fps 帧序号 Fxxxx（对应时间 xxxx * 100ms）或静止折叠区间 Fxxxx~Fyyyy [HOLD xN] / [BLANK xN]，右侧为画面底部裁剪条带。
请提取所有烧录的前景中文字幕，严格按画面字面原文输出（保留括号、省略号、空格与连字符，不要自动纠错或简繁转换；忽略背景中的英文报纸/铭牌道具字，若遇到滚动水印请将 category 标为 watermark）。
仅输出 JSON 数组：
[
  {"start_frame": 260, "end_frame": 287, "category": "dialogue", "text": "我流血啦…我死啦"}
]
```
