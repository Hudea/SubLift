# ADR-0042（草案）Det 输入几何：从逐字节 parity 转为质量门

> **状态**：提议（proposal），2026-10-21；2026-10-22 采纳工作决策 C，未批准。批准后并入
> [`docs/DECISIONS.md`](../DECISIONS.md) 作为 ADR-0042。
> **证据**：[`det-input-geometry.md`](det-input-geometry.md)、
> [`det-pixel-budget-delivery.md`](det-pixel-budget-delivery.md)。
> **关联**：Phase 16 D04 要求后续加速方向另立 Phase 并给出"目标阶段占比 + 预计移动毫秒数"；
> 本提案即为 det 维度的机制说明。

## 1. 背景

Phase 6 的 Python → C++ cutover 建立了一条 parity 契约：C++ 产品的 SRT 必须与冻结的
Python/RapidOCR Oracle 逐字节一致。该契约由 `scripts/parity/check_paddle_gate.py` 的
`source_output_sha256_exact` 门执行，并在 ADR-0028 中被扩展为"任何只提升微基准、但改变
端到端字幕 hash 的执行策略都不得进入产品默认"。

这条契约在迁移期是正确的：它用最低的判断成本证明了 C++ 没有改变行为，并实际拦下了
Rec batch=1 等会改变 Latin 输出的微优化（ADR-0028）。但它是一个**迁移等价契约**，
不是产品质量规格。

当前证据：

- Phase 16 归因基线：`det_infer` 占 core wall 的 **84.35%**，是唯一的结构性上限。
- 根因不是模型慢，而是 RapidOCR 的 `limit_side_len=736, limit_type=min`：1920×324 的
  bottom-crop 被放大到 4352×736（3.203MP），成本与源分辨率无关。
- 0.7MP 像素预算实验：固定负载 1080p 端到端 120.26s → 41.50s（2.90×；独立复核
  136.35s → 45.00s，3.03×），det 单次 436.9ms → 92.3ms（4.73×）。
- 质量（官方 one-to-one）：5 个素材组合 CER macro 全部下降（1080p 17.27% → 3.67%，
  mixed_multiline 55.12% → 7.50%），exact 全部上升；但 1080p/720p split 增加
  （1→4、0→3），720p/480p timing precision 各降约 1pp，latin precision 39.1% 仍低。
- 2026-10-22 策略探索：30% 带上 0.6MP 的 manifest 聚合 precision 87.0%、F1 87.4%，
  优于 0.7MP 的 81.0%、84.3%；25% 带上短边 320 同时改善 CER 与 timing；720p 是 0.6 的
  反例（precision 89.4% vs 95.1%）。见
  [`det-geometry-policy-exploration.md`](det-geometry-policy-exploration.md)。

切换默认值会改变每一条 det 框与 SRT，因此 `source_output_sha256_exact` **必然失败**。
这不是回归，而是"有意改变输出"。

**已存在的基线偏离（2026-10-21 发现）**：冻结 Oracle 基线的 `oracle_commit` 是
`544426e`（2026-07-30），而 C++ 在 `2fe2f91`（2026-10-04）加入了相邻碎段字幕去重合并。
同一 1080p 负载上，当前 C++ 736 输出已与冻结 Python Oracle 不同：Python 把
"跟胡尼克，一位平凡的狐狸"拆成 3 条，C++ 合并成 1 条；CER 17.27% vs 18.99%。
该差异来自**后处理层**，与 det 几何无关。因此 byte-exact 门在 det 切换之前就已经不成立；
本 ADR 的契约决策必须把 det 几何与合并策略一起纳入质量治理，而不是只处理 det。

## 2. 问题

Python Oracle 是 **736 参考实现**，不是产品规格。它无法表达"像素预算"策略；要生成
0.7MP 的参考输出，必须修改冻结 Oracle 本身。于是当前系统只有两种状态：

1. 保持 736：parity 门永远通过，但已知的 3× 收益和更好的文本质量无法进入产品。
2. 切换几何：质量更好、速度更快，但 parity 门必红，且没有"证明变好之后可以变"的流程。

问题不在"要不要逐字节"，而在于**契约缺少有意的、可审计的变更通道**。

## 3. 决策（提议）

1. **把 det 输入几何从 parity 维度移出，改为质量门维度。** C++ 允许在这一维度与历史
   Python Oracle 产生输出差异，但必须通过质量阈值与覆盖矩阵。同时确认 `2fe2f91`
   引入的字幕合并策略已属于质量治理维度，二者在 D02 共用或分别建立基线。
2. **保留 parity 的维度**：SRT 文本格式/时间码/编码、非 det 的配置与状态语义、模型与
   ORT 二进制指纹、错误与 fail-closed 行为。这些维度仍要求逐字节或逐元素一致。
3. **几何策略采用双下限（工作决策 C）**：

   ```text
   r           = max(w, h) / min(w, h)
   target_area = max(0.6MP, 320^2 × r)
   ratio       = max(1, sqrt(target_area / (w × h)))
   output      = 32 倍数对齐(w × ratio, h × ratio)
   ```

   短边不足 320 才放大，面积不足 0.6MP 才放大，两者取更大的目标；永不缩小。
   - 1080p 30% → native 1920×320（0.61MP）；720p/480p 30% → 1888×320（0.60MP）；
     25% → 2272×320（0.73MP）；22.5% → 2528×320（0.81MP）；≤20% 由 global padding
     定在 1920×480（0.92MP）。
   - 默认 bottom-crop 带高 ≤30%（`region_bottom_ratio=0.3`）；40%+/full-frame 只属于
     ROI/全帧路径，不参与默认决策。
   - 回退规则：若 720p/低分辨率 30% 带的 timing precision 相对 736 掉超过 1pp，短边下限
     提到 352（C'），但短边 ≥320 的源保持 native；该回退由 D03 数据确认。
4. **质量门**（替代 det 维度的字节相等，具体阈值在 D02 冻结）：
   - CER macro/micro 不得比冻结基线差超过 1pp；
   - exact 不得下降；
   - usable recall 不得下降超过 2pp；
   - timing F1 / timing precision 不得下降超过 1pp；
   - noise / empty 计数不得增加；
   - split / false alarm 必须报告，并对增量给出解释。
5. **重冻基线**：更新 `benchmark/parity/goldens/paddle/paddle_quality_baseline.v1.json`
   与受影响的 benchmark baselines，记录新的 `oracle_commit`、模型指纹、输出 hash 与
   gate 结果；旧基线保留为归档，不删除。
6. **回滚**：保留 736 作为可配置的回退值，D04 必须演练"一键回 736 并恢复
   `56b1b08a…`"；回滚不依赖重新构建。
7. **历史 Oracle 的定位**：`python-product-last` 标签与冻结 Oracle 继续用于审计与
   回归复现，但不再是 det 几何维度的默认验收标准。

## 4. 后果

**正面**

- 固定负载端到端约 3×，det 占比从 84.35% 降到约 55%；
- 文本 CER/exact 显著改善，且改善不是旧脚本匹配口径造成的假象；
- 为后续 det-once / rec-many 等结构性优化腾出预算。

**负面**

- 产品输出不再与历史 Python Oracle 逐字节一致，依赖该假设的脚本/文档需要同步更新；
- gate 工具链、baseline 与部分 golden 需要重冻；
- 720p/480p 的 timing precision 与 1080p/720p 的 split 有轻微退步，需要在质量门中显式
  接受或缓解；
- 0.7MP 在 480p 扫描中选定，存在调参集复用，必须用未参与调参的素材复核。

**中性**

- Oracle 仍保留为历史审计参考；这不是"删除 Oracle"，而是"改变它在 det 维度的角色"。

## 5. 备选方案

- **保持 736**：零契约风险，但放弃已验证的 3× 与文本质量改善；仅适合作为"暂不决策"的
  默认状态。
- **修改 Python Oracle 以支持像素预算**：需要改冻结参考本身，且无法再证明"历史行为"，
  成本高于直接建立质量门。
- **只做 det-once + rec-many**：与几何正交，可作为后续独立 Phase；不能替代本次决策。

## 6. 未决问题

- 短边下限取 320（C）还是 352（C'）；由 720p 与 22.5–25% 带的 D03 证据决定。
- 合并策略（`2fe2f91`）与 det 几何共用一套质量基线，还是分别独立重冻？
- full-frame、ROI、竖屏、超宽条带的几何是否需要单独策略。
- timing precision 的阈值是否按素材/时长加权，避免小样本主导。
- 旧 golden 的归档位置与命名。
- 回滚开关放在 CLI/config 还是仅内部配置。

## 7. 验收与落地

本提案由 Phase 18 承接，D01 完成 ADR 批准与配置面，D02 完成 gate 重放与基线重冻，
D03 完成覆盖矩阵与全门质量验证，D04 完成默认切换与回滚演练。执行计划与素材矩阵见
[`det-geometry-evidence-plan.md`](det-geometry-evidence-plan.md)。在 D01 批准之前，产品
默认值保持 736，`SUBLIFT_DET_PIXEL_BUDGET` 仅作诊断开关。
