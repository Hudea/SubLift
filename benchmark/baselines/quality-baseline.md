# 固定 GT 质量基线

> **状态：** 当前质量回归锚（feat-034 P1 fix2）
> **用途：** 判断任何打轴、OCR、行选择、共识或去重改动是否回退质量
> **来源：** 2026-07-10 的已验收 `feat034_p1_fix2` 原始 benchmark 产物（本地 `debug/`）

## 固定负载

| 项 | 固定值 |
|---|---|
| 视频 | Zootopia hard-subtitle clip，1920×1080，254.272 s（视频本体不入库） |
| Ground truth | `benchmark/datasets/Zootopia_clip_1080p_gt.srt`，87 条 |
| 采样 | 5 fps |
| 区域 | source-frame `[0,848,1920,87]` |
| OCR | Apple Vision，`zh-Hans` + `en-US` |
| 文字画像 | `subtitle_script=cjk`，行级选择开启 |
| 诊断 | temporal IoU 一对一匹配，阈值 0.5；usable 的 CER 阈值 0.20 |

## 验收结果

| 指标 | 验收门 | 实测 | 结果 |
|---|---:|---:|:---:|
| timing recall | — | 96.6% | — |
| timing precision | ≥98.8% | 98.8% | 通过 |
| timing F1 | ≥95.2% | **97.7%** | 通过 |
| CER macro | ≤6.6% | **3.2%** | 通过 |
| CER micro | — | 2.4% | — |
| 字符准确率 | — | 97.6% | — |
| usable subtitle recall | ≥85.1% | **92.0%** | 通过 |
| text.noise | ≤2 | **0** | 通过 |
| text.empty | ≤1 | **0** | 通过 |
| 检测条目数 | — | 85 / 87 GT | — |

这一锚点证明当前固定负载下的质量水位；不应用它反向推导对新片源的质量承诺。

## 仍保留的失败簇

| 类型 | 数量 | GT 示例 | 后续方向 |
|---|---:|---|---|
| `text.high_cer` | 4 | 19、60、68、74 | 字幕区质量或文本规范化 |
| `timing.fn.merged_into_neighbor` | 2 | 69、70 | CHANGE / 长稳定段拆分 |
| `timing.fn.no_overlap` | 1 | 15 | presence、采样或短字幕机制 |
| `timing.fp.false_alarm` | 1 | 68 | 前景检测或 OCR 过滤 |

这些残留不是放宽质量门的理由；新 feature 应先说明它影响哪个 failure cluster，再以同一
口径回归此基线。

## 使用规则

1. 质量改动至少报告上表六个验收指标及 failure cluster 变化。
2. 真实 Vision 的多次运行还必须记录 `detection_hash`；hash 改变时先解释结果变化，再讨论
   速度收益。
3. 新增英文、混排或不同位置 GT 后，应另立多源基线，不覆盖本报告的单源锚点。

复现入口与产物说明见 [benchmark README](../README.md)；完整实现/收口证据见
[feat-034](../../docs/phases/phase3.json)。
