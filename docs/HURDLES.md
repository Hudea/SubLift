# 开发障碍与解决记录 (Hurdles)

记录开发过程中遇到的关键技术问题、排查路径及最终解决方案，为后续开发与排查提供参考。

## 模板

```markdown
### [问题简述]
- **日期**：YYYY-MM-DD
- **现象**：[问题发生时的具体表象、错误日志、复现条件]
- **排查路径**：
  1. [尝试了什么方向]
  2. [发现了什么线索]
- **根本原因**：[导致问题的深层原因]
- **解决方案**：[最终采取的解决方式]
- **相关提交/文件**：[涉及修改的核心文件或 commit]
```

---

### dHash 对中文文本内容变化判别力不足，导致 timeline 漏分段
- **日期**：2026-07-03
- **状态**：未解决（端到端实测确认，待优先级评估）
- **现象**：用 `scripts/test_timeline.py` 对 `debug/Zootopia_clip_hardsub1.mkv`（00:01:00~00:02:00 窗口）实测，段 33（00:01:47→00:01:59, 11600ms）合并了实际 4 个独立字幕段：
  - "能不顾他们惊人的差异"
  - "彻底化解偏见和刻板印象"
  - "那也许我们都能接受彼此的差异"
  - "一起成为更好的动物"
  
  这 4 句字幕的 dHash 值非常接近（都 1692... 开头），前景占比也相似，状态机判定为"内容未变化"，未触发 CHANGE 事件。
  
  **端到端 Pipeline 实测复核（5fps，2026-07-03）**：在完整 Pipeline 跑通后再次验证，该问题复现为段 22（13s 段）合并实际 4 段。端到端整体召回/精确率 91.3%（23/23 实际段中正确识别 21 段），此 dHash 漏检贡献了主要的 recall 损失。
- **排查路径**：
  1. 确认状态机逻辑正确：CHANGE 事件依赖 `hamming_distance(signature.dhash, anchor.dhash) > change_threshold`，threshold=10
  2. 确认 dHash 计算正确：9×8 降采样后比较相邻像素亮度梯度，64bit
  3. 分析根因：9×8 降采样把汉字笔画细节平均掉，两句长度相近的中文字幕在 9×8 网格上结构几乎同构，dHash 距离 < 10
- **根本原因**：dHash 设计初衷是检测"通用场景/结构变化"（对整体亮度漂移免疫、对结构变化敏感），但对"文本内容变化"这种**高频局部细节差异**判别力不足。降采样到 9×8 丢失了汉字笔画的高频信息，使不同文本的哈希趋同。
- **待评估方案**：
  1. **加 pixel-diff 补充信号（首选）**：在 signature 里除 dHash 外，加一个与锚帧的全分辨率像素差异比例（变化像素占比）。dHash 检测结构变化，pixel-diff 检测内容变化，两信号 OR 触发 CHANGE。改动小，最可能有效——pixel-diff 对全分辨率敏感，中文笔画差异不会丢失。这正好是最初设计提案中的方案，被参考实现的 dHash 覆盖了，但两者实为互补关系。
  2. **加大 hash_size**（16×16=256bit）：保留更多细节，但仍是降采样，对中文笔画效果有限。
  3. **换 pHash/aHash**：不同哈希算法，但本质问题（降采样丢高频）相同。
- **相关文件**：`src/sublift/pipeline/signature.py`（dHash 计算）、`src/sublift/pipeline/changepoint.py`（CHANGE 事件触发）、`scripts/test_timeline.py`（实测脚本）、`debug/timeline_compare_result.txt`（比对结果）

---

### OCR 锚帧落在字幕过渡画面导致空文本
- **日期**：2026-07-03
- **状态**：未解决（端到端实测发现，待评估）
- **现象**：端到端 Pipeline 5fps 实测，段 5、段 11 出现 OCR 空文本。锚帧（IN/CHANGE 事件触发帧）恰好在字幕过渡画面（淡入/淡出/切换瞬间），Vision 未识别出文字，该段 text 为空。
- **排查路径**：
  1. 确认 OCR 引擎正常：同一视频其他段识别成功
  2. 确认锚帧选取逻辑：core.py 用 `anchor_frames[event.timestamp_ms]` 取事件触发帧，该帧是状态机确认变化的帧
  3. 分析根因：状态机用迟滞确认（hysteresis_frames=2），事件 timestamp_ms 回溯到信号首次出现帧，但该帧可能正是字幕过渡帧（半透明、不完整）
- **根本原因**：锚帧选取策略只考虑"变化首次出现"，未考虑该帧是否是"字幕稳定可读帧"。过渡帧（淡入未完成、切换瞬间）OCR 置信度低或识别不出文本。
- **待评估方案**：
  1. **锚帧延后 N 帧（首选）**：事件触发后，往后取 1-2 帧作为 OCR 锚帧（字幕已稳定）。改动小，core.py 在缓存 anchor_frame 时取 `当前帧 + offset`。
  2. **多帧 OCR 取最优**：对每段取多帧 OCR，选 confidence 最高的。增加 OCR 调用，但精度更高。
  3. **空文本段重试**：OCR 返回空时，自动取段内下一帧重试。fallback 策略。
- **影响评估**：端到端实测 2/23 段空文本（8.7% 段丢失文本），属可接受范围但影响最终 SRT 质量。与 dHash 漏检叠加，整体 recall 91.3%。
- **相关文件**：`src/sublift/pipeline/core.py`（anchor_frames 缓存与 OCR 调用）

---

### Pipeline 端到端 5fps 实测基线（2026-07-03）
- **测试条件**：`debug/Zootopia_clip_hardsub1.mkv`，00:01:00~00:02:00 窗口，5fps 采样，VisionOcrEngine（zh-Hans+en-US）
- **结果**：23 个实际字幕段，正确识别 21 段，**召回率/精确率 91.3%**，**0 误检**
- **缺陷分布**：
  - 2 段 OCR 空文本（段 5、11）：锚帧落在过渡画面（见上条 HURDLE）
  - 1 段合并 4 段（段 22）：dHash 对相似中文文本判别力不足（见首条 HURDLE）
- **结论**：Pipeline 端到端在 5fps 下表现扎实，打轴状态机很干净（0 误检）。主要瓶颈在 OCR 精度（空文本、英文水印干扰）和长段合并（dHash 对相似内容的区分度不够）。对 Phase 1 MVP 是不错的起点。
- **相关文件**：`scripts/test_timeline.py`、`debug/timeline_compare_result.txt`

---

### 字幕区域裁剪过宽导致 OCR 质量低
- **日期**：2026-07-04
- **状态**：未解决（feat-011 端到端 benchmark 发现）
- **现象**：CLI 端到端 benchmark（1080p, 5fps, vision），63 条导出中 OCR 字符准确率仅 22.5%（1-CER）。逐条对比显示前 6 条 CER=0% 完全正确，第 7-10 条 CER=131~525% 严重偏离。
- **排查路径**：
  1. 分析逐条对比文件（`debug/cli_comparison.txt`），发现偏离条目的检测文本含 `DEVELOPING`、`LIKELY DUO UNCOVERS CONSPIRACY`、`PRISONZNN` 等英文
  2. 这些英文是画面上方的新闻标题/字幕栏，不是目标中文字幕
  3. 抽帧分析字幕实际位置：字幕带在 y=860~950（画面 80~87% 区域），而 `bottom_ratio=0.3` 裁剪 y=756~1080（下部 30%），多带了 234px 非字幕噪声区域
- **根本原因**：`BottomCropDetector` 默认 `bottom_ratio=0.3` 是通用默认值，未针对实际视频字幕位置校准。裁剪区域过宽把画面上方的英文新闻标题也包含进来，Vision 把英文标题误识别为字幕，导致 OCR 文本严重偏离真实 SRT。
- **待评估方案**：
  1. **调小 bottom_ratio（首选）**：从 0.3 调到 0.13~0.15（y=918~1080 或 y=936~1080），精准对齐字幕带。改动小，只需改 Config 默认值或 CLI 加 `--region-ratio` 参数。
  2. **CLI 加区域参数**：加 `--region-ratio` / `--region-box` 参数让用户指定裁剪区域。灵活性高但增加 CLI 复杂度。
  3. **自适应区域检测**：用 Vision 的文字检测框自动定位字幕区域。精度最高但实现复杂，属 Phase 2 增强。
- **影响评估**：这是当前 OCR 质量低的主因，前 6 条（无英文标题干扰）CER=0% 证明 OCR 引擎本身精度足够。调准区域后预计字符准确率可大幅提升。
- **相关文件**：`src/sublift/detector/bottom_crop.py`、`src/sublift/config.py`（`region_bottom_ratio=0.3`）、`debug/cli_comparison.txt`（逐条对比证据）


