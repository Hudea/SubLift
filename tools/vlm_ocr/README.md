# `tools/vlm_ocr` — 独立多模态大模型硬字幕 OCR 与算法验证工具

用于在**零修改 `cpp/` 核心架构**的前提下，将 `sublift_cli --timing-only` 产出的高可靠时间槽送入多模态大模型（如 `.env` 中配置的 `MiniMax-M3.1-Flash`）进行高精度硬字幕转录，并通过唯一权威评估器 [`scripts/srt_eval.py`](../../scripts/srt_eval.py) 与本地 PaddleOCR 及真源 GT 对标。

## 架构与边界约定
1. **选区与打轴零重写**：全部委托给 [`tools/cli/main.cpp`](../cli/main.cpp) 的 `sublift_cli --timing-only`（支持 `--auto-region` 或 `--region`）。
2. **评估零重写**：全部复用 [`scripts/srt_eval.py`](../../scripts/srt_eval.py)。
3. **模块化可演进**：内部拆分为 `timing_adapter`（对齐 `sublift_cli`）、`sheet_builder`（中点取帧 + 单列编号拼图）、`vlm_client`（OpenAI 兼容并发 + 自适应思考档位 + 单表失败二分自救 + `.partial.json` 断点续传）、`assembler`（纯函数 Schema 校验与装配）、`evaluator`（指标汇总）。

## 环境变量配置（`.env`）
工具会自动读取仓库根目录的 `.env`（命令行显式参数优先级最高，进程环境变量次之，`MINIMAX_*` 作为内置默认）：

```env
MINIMAX_API_KEY=...
MINIMAX_API_BASE_URL=https://api.minimax.cn/v1
MINIMAX_MODEL=MiniMax-M3.1-Flash
```

## 常用命令

### 1. 单视频提取 + 自动选区 + 与本地 PaddleOCR / GT 对比
```bash
python3 -m tools.vlm_ocr \
  debug/smoke-test-data/Zootopia_clip_1080p.mp4 \
  --out-dir debug/vlm_ocr_validation/zootopia_1080p \
  --fps 5.0 \
  --workers 4 \
  --compare-paddle \
  --gt-srt debug/smoke-test-data/Zootopia_clip_1080p.srt
```

### 2. 指定字幕 ROI 与既有打轴输入
```bash
python3 -m tools.vlm_ocr video.mp4 \
  --region 0,778,1920,121 \
  --timing-input path/to/timing.jsonl \
  -o debug/out/result.vlm.srt
```

### 3. 目录批量处理
```bash
python3 -m tools.vlm_ocr debug/my_videos/ \
  --out-dir debug/vlm_ocr_validation/batch_run \
  --fps 5.0 \
  --compare-paddle
```
