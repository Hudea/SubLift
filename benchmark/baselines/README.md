# 版本化 benchmark 基线报告

本目录保存已经验收、可在代码审查中阅读的**基线结论**，而不是每次运行自动生成的原始
产物。原始 `.agent.json`、CSV、trace 与本地视频继续放在被忽略的 `debug/`，避免绝对路径、
临时运行噪声和重复字幕文本进入版本库。

| 报告 | 作用 | 当前产品意义 |
|---|---|---|
| [质量基线](quality-baseline.md) | 固定 GT 的质量锚 | 所有影响打轴、OCR、行选择或去重的改动必须回归 |
| [性能归因基线](performance-attribution-baseline.md) | 当前默认 ROI 通路的 clean-commit 性能与阶段归因 | 只用于同机同负载优化决策，不作为跨机器或跨片源承诺 |

两份报告均使用 Zootopia 1080p 固定片段、5fps、Apple Vision、固定 region
`[0,848,1920,87]`、`subtitle_script=cjk`。该数据集是回归锚，不代表英文、中英混排、不同
字幕位置或其它片源的泛化能力。

重跑请使用 `uv run sublift-benchmark` 和 `benchmark/configs/`，输出默认写入
`debug/benchmark/`；只有经过相同协议验收并明确取代现有基线时，才更新本目录的报告。
