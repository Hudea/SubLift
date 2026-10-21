# Det 几何切换：切换前证据计划

> **状态**：执行计划（proposal），2026-10-21。Phase 18 D03 的输入；本文件不是验收证据，
> 证据结论写入 Phase 18 D03 `evidence`。
> **关联**：[ADR-0042 草案](adr-det-geometry-contract.md) ·
> [几何实验](det-input-geometry.md) · [交付与复核](det-pixel-budget-delivery.md)。

## 1. 要回答的三个问题

1. **归因**：736 下的"框破碎/半行丢失"是几何造成的，还是 C++ DB 后处理移植偏差？
2. **质量**：0.7MP（或 native 面积策略）在全部产品路径与未参与调参的素材上，是否仍然
   "文本 CER/exact 不劣、条目级指标可接受"？
3. **可执行性**：质量门、历史 Oracle 重放与一键回滚是否真的能跑通？

在 Q1–Q3 有可复核答案之前，产品默认保持 736。

## 2. 已有结论（2026-10-21）

**E1a：mixed_multiline 是干净的几何对照（已完成）**

- 历史 Python Oracle（`python-product-last` @ `782b876`；rapidocr 3.9.2 / ORT 1.28.0 /
  OpenCV 4.13.0.92，与历史 `uv.lock` 一致）与当前 C++ 在
  `--engine paddle --fps 5 --confidence 0.5 --script cjk` 下分别运行。
- Python 736 与 C++ 736 的 SRT 归一化文本完全相同；两者 CER macro 55.12%、exact 20.0%、
  recall/precision 100%；C++ 0.7MP 为 CER 7.50%、exact 90.0%。
- **结论**：该 fixture 的"缺左半段/少一行"来自 736 几何，不是 C++ DB 后处理独立引入的偏差。

**E1b：整段 SRT 不是干净的几何对照（已完成，结论修正）**

- Zootopia 1080p：Python 736 为 85 条、CER 18.99%、precision 84.7%；C++ 736 为 81 条、
  CER 17.27%、precision 90.1%。差异来自合并层：Python 把
  "跟胡尼克，一位平凡的狐狸"拆成 3 条，C++ 合并成 1 条（`2fe2f91`）。
- latin：实验的 C++ 736 用 5fps，而源 manifest 是 2fps；且 Python 未应用与 C++ 相同的
  脚本/合并策略，73 条 vs 41 条、CER 71.73% vs 37.62%，不可直接比较。
- Python 736 的 Zootopia 指标与冻结 `paddle_quality_baseline.v1.json`（CER 18.99%、
  precision 84.7%）一致，说明重放本身可信；差异在 C++ 合并层，不在 det。
- **结论**：整段 SRT 混入了与几何无关的合并策略差异，不能作为 det 几何的因果证据。
  几何问题应改用 det box 级对照（E1c），或在合并层不活跃的素材（mixed）上判定。

**关键事实**：冻结 Oracle 基线（`544426e`，2026-07-30）早于 C++ 合并层
（`2fe2f91`，2026-10-04）。因此 `source_output_sha256_exact` 在 det 切换之前就已经
不成立；Phase 18 D02 必须把合并策略与 det 几何一起纳入新基线，或明确分层。

## 3. 工作项

| 项 | 目的 | 状态 | 产物 |
|---|---|---|---|
| E1 | Python/RapidOCR 736 与 C++ 736 对照，排除移植偏差 | done（mixed 干净；zoo/latin 受合并层干扰） | mixed 文本一致 + 诊断分数 |
| E1c | det box 级对照（Python RapidOCR 736 vs C++ det 736） | todo | box IoU/recall + 可视化 |
| E2 | manifest 三源 736 parity 重放；确认历史 Oracle 可复现 | todo（需先分离合并层） | 三源 SRT sha256 对照 |
| E3 | 路径覆盖：bottom-crop / full-frame / ROI / 竖屏 / 超宽 / 真实低分辨率 | todo | 每路径 A/B + 质量分数 |
| E4 | 未参与调参素材上的 0.4–0.8MP 平台复核 | todo | 扫描表 + 策略建议 |
| E5 | 策略二选一：固定 0.7MP vs `clamp(native, 0.6MP, 1.0MP)` | todo | D01 决策记录 |

依赖：E2 的 gate 自动化依赖 D02 的 gate 语义改造；在 D02 完成前用"Python/C++ CLI 手动
重放 + `analyze_entries` 评分"作为过渡证据。

## 4. 素材矩阵

| 路径/素材 | 现有 | 缺口 | 处理 |
|---|---|---|---|
| bottom-crop 1080p 真实 CJK | `debug/Zootopia_clip_1080p.mp4` | 无 | E1/E2 已用 |
| bottom-crop 720p/480p | 实验下采样 | **同源，不是独立内容** | 只作分辨率敏感性，不作独立质量证据 |
| bottom-crop latin/mixed | `debug/paddle-quality/*` | 无 | E1/E4 |
| full-frame | 无固定样本 | 需确定触发方式（server region_box 或 pipeline 配置） | E3 |
| ROI | Phase 16 的 `debug/ab-phase16/` 流程 | 需按新几何重跑 | E3 |
| 竖屏 / 超宽 | 无 | 需真实或明确标注的合成素材 | E3 |
| 真实低分辨率 | 无 | 需屏幕录制/真实片源 | E3，禁止用下采样冒充 |
| 未参与调参素材 | 无 | 至少 2 个独立内容源 | E4 |

## 5. 命令

历史 Oracle（不修改当前 `src/`）：

```bash
REPO=$(git rev-parse --show-toplevel)
git worktree add --detach /tmp/sublift-python-oracle python-product-last
cd /tmp/sublift-python-oracle
PYTHONPATH=src "$REPO/.venv/bin/python" -m sublift extract \
  "$REPO/debug/Zootopia_clip_1080p.mp4" -o /tmp/py-zoo-736.srt \
  --engine paddle --runtime python --fps 5 --confidence 0.5 --script cjk
```

当前 C++：

```bash
cd "$REPO"
SUBLIFT_WORKER_PATH=build/cpp-rel/bin/sublift_worker \
  build/cpp-rel/bin/sublift extract debug/Zootopia_clip_1080p.mp4 \
  -o /tmp/cpp-zoo-736.srt --engine paddle --fps 5 --script cjk

SUBLIFT_DET_PIXEL_BUDGET=700000 SUBLIFT_WORKER_PATH=build/cpp-rel/bin/sublift_worker \
  build/cpp-rel/bin/sublift extract debug/Zootopia_clip_1080p.mp4 \
  -o /tmp/cpp-zoo-budget.srt --engine paddle --fps 5 --script cjk
```

评分（官方 one-to-one）：

```bash
PYTHONPATH=src python3 - <<'PY'
from pathlib import Path
from sublift.benchmark.srt import load_srt
from sublift.benchmark.diagnostics import analyze_entries
for det in ('/tmp/cpp-zoo-736.srt', '/tmp/cpp-zoo-budget.srt'):
    d = analyze_entries(load_srt(Path(det)),
                        load_srt(Path('benchmark/datasets/Zootopia_clip_1080p_gt.srt')),
                        temporal_iou_threshold=0.5)
    m = d.metrics
    print(det, m.recognition.cer_macro, m.recognition.exact_match_rate,
          m.timing.timing_f1, m.timing.split_count)
PY
```

## 6. 验收阈值（与 ADR-0042 一致，D02 冻结）

- E1：在合并层不活跃的素材上，Python 736 与 C++ 736 的归一化文本相同；或用 det box 级
  IoU/recall 证明几何一致。不得用受合并层影响的整段 SRT 作为几何结论。
- E2：三源 736 SRT sha256 相同；若不同，必须先分离合并层（`2fe2f91`）与 det 几何，
  记录基线偏离，不得笼统归因为"det 移植偏差"。
- E3：`paddle_quality` 全门通过；CER macro/micro 不比基线差 >1pp；exact 不下降；
  usable recall 不下降 >2pp；timing F1/precision 不下降 >1pp；noise/empty 不增加；
  split/FP 增量必须有解释。
- E4：在未参与调参的素材上，0.4–0.8MP 至少 3 个点落在同一质量平台；否则不做默认切换。
- E5：策略选择必须写明 1080p native 与低分辨率上采样的取舍，并有数据支撑。

## 7. 风险与边界

- Python Oracle 依赖历史 tag + 当前 `.venv` + `~/.cache/sublift/rapidocr-models`；
  版本漂移会导致对照失败，必须记录实际版本。
- 单次运行的计时噪声：性能对比至少 2 次，质量对比以 SRT 为准。
- 下采样/合成素材不能替代真实低分辨率与真实竖屏证据。
- **合并层混淆**：C++ 在 `2fe2f91` 引入的字幕合并会改变整段 SRT；任何"Python vs C++"
  的整段对照都必须先确认合并层是否介入，否则会把后处理差异误判为 det 几何差异。
- E2 的 gate 自动化在 D02 前不可用；手动评分只作过渡，不能冒充 gate 通过。
- `debug/` 产物不入库；证据以命令、哈希、分数和结论写入 Phase 18 D03。
