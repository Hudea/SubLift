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

### Apple Vision 对 CoreText 合成图返回零 observations
- **日期**：2026-07-30
- **状态**：已解决（默认开发门稳定；live smoke 保留为显式诊断）。
- **现象**：相同 `VisionOcrEngine` 在当前 macOS/Vision revision 上可正常通过固定视频
  GT，但对测试内 CoreText 绘制的 “SUBLIFT” 合成图稳定返回空 observations，导致全量
  CTest 出现单个环境相关失败。
- **排查路径**：
  1. 单独重复运行失败用例，确认不是并发或偶发内存问题。
  2. 对照固定视频 GT 与几何契约，确认真实输入质量门和确定性转换逻辑仍通过。
  3. 检查测试契约，发现它把 OS 框架对合成图的启发式结果当成了 L0 硬断言。
- **根本原因**：Apple Vision 的 live OCR 是 OS 版本相关的外部能力；合成字体图是否产生
  observations 不属于项目可控制的确定性契约。
- **解决方案**：将空白输入、语言配置等确定性契约保留在默认 `[vision][ocr]` 测试，
  将英文/中文/像素格式/重复调用的 live synthetic smoke 移至隐藏
  `[.vision-live]` 标签；固定视频 live GT 继续作为扩展正确性门。
- **相关文件**：`cpp/tests/vision_ocr_test.mm`、`docs/cpp/phase6.4-vision.md`。

---

### Paddle Det 仅 1px quad 漂移却触发错误 180° 分类
- **日期**：2026-07-30
- **状态**：已解决（feat-06805 / ADR-0027）。
- **现象**：Phase 6.8 首轮真实三源质量门中，C++ 与 Python 只在一条 CJK 字幕上出现
  CER 超限。上游 Det quad 只有 1px 差异，但同一 upright crop 的 Cls 180° score 从
  Python `0.6868` 变为 C++ `0.9383`，跨过 `0.9` 阈值后错误旋转，最终文字发生变化。
- **排查路径**：
  1. 用冻结 stage trace 对比 Det tensor、probability map、quad、crop、Cls tensor/score
     和 Rec 输出，确认神经网络输入与概率图不是主要差异。
  2. 固定 Python Det quad 重放 C++ Crop/Cls/Rec，所有下游 tensor/text 恢复 exact，
     将问题隔离到 DB unclip 几何。
  3. 对比 RapidOCR/pyclipper 与 C++ offset，发现近似浮点矩形扩张没有复刻 Clipper 6 的
     float→integer、round join、arc tolerance 和二次 `minAreaRect` 语义。
  4. 没有通过提高 Cls 阈值或放宽 CER/坐标门掩盖问题，而是新增 rotated-contour 黑盒回归。
- **根本原因**：方向分类器对 crop 边缘、背景和透视几何高度敏感；1px quad 误差不是
  “无害的坐标尾数”，它会被 crop 和 Cls 非线性放大。C++ unclip 的几何算法与 Python
  Oracle 并非真正等价。
- **解决方案**：精确复刻 pyclipper/Clipper 6 的整数 round offset、round join、
  `arc_tolerance=0.25` 与二次 `minAreaRect`，并保持 Cls `score > 0.9` 契约不变。
- **验证结果**：9/9 Det fixture quad 坐标 `max_abs=0`，box recall/mean IoU=1.0；
  三来源 Python/C++ SRT SHA256 逐源 exact，全部质量指标 delta=0。
- **相关文件**：`cpp/src/paddle/ppocr_db_postprocess.cpp`、
  `cpp/tests/paddle_db_test.cpp`、`scripts/parity/check_paddle_det_parity.py`、
  `scripts/parity/check_paddle_gate.py`、`docs/DECISIONS.md` ADR-0027。

---

### 相同 ONNX Runtime 版本与 provider 仍出现显著 Paddle 性能和数值差异
- **日期**：2026-07-30
- **状态**：已解决（feat-06803、feat-06806 / ADR-0025、ADR-0028）。
- **现象**：算子正确性收敛后，C++ Paddle 在质量冻结点仍比 Python 慢约
  `1.14–1.15x`；更早的 Phase 6.7 简化实现约为 Python `2.74x`。Python wheel 与
  Homebrew 都报告 ONNX Runtime 1.28.0、CPUExecutionProvider，却同时存在性能差距和
  Det probability map `1.9729137420654297e-5` 的最大尾数差异。
- **排查路径**：
  1. 建立 input、Det preprocess/infer/postprocess、crop、Cls、Rec 和 output 十阶段 trace，
     排除完整 DB 与图像前后处理是修复后剩余回退的主因。
  2. 记录实际加载的 ORT dylib SHA，而不只看版本字符串和 provider。
  3. 让 C++ Candidate 改用 Python wheel 随带的同一官方 dylib；Det probability map
     变为逐元素 exact，Det/Rec 速度追平或领先。
  4. 执行 ORT intra-op 1/2/4/default sweep，aggregate median 分别为
     `1448.356/856.939/615.711/649.736ms`。
- **根本原因**：ONNX Runtime 的版本号和 provider 名称不足以定义实际 kernel、编译选项与
  归约行为；不同发行来源的同版本二进制既可能产生浮点尾数差异，也可能具有显著不同的
  CPU 性能。
- **解决方案**：canonical 门固定官方 ORT dylib SHA
  `cadd9517e089d197e5d381bc31813875eb8addce8712bac501645fea8008800c`，
  使用 4 个 intra-op threads；CMake 将该 dylib 复制到 build `lib/` 并写相对 rpath。
  同二进制保留严格 `1e-5` probability 门，跨构建诊断允许指纹化 `2.5e-5`，但 box、
  score、文本和产品输出门不放宽。
- **验证结果**：120s canonical Python/C++ wall median `50.700/45.407s`
 （`0.8956x`），RSS `1864.406/1706.297MiB`（`0.9152x`）；移除虚拟环境 ABI
  软链后 probe、22 个 Paddle cases 与最终 cutover 门仍通过。
- **相关文件**：`cpp/src/paddle/paddle_models.cpp`、
  `scripts/parity/check_paddle_det_parity.py`、`scripts/parity/check_paddle_perf.py`、
  `cpp/CMakeLists.txt`、`docs/DECISIONS.md` ADR-0025/ADR-0028/ADR-0029。

---

### Rec batch=1 微基准更快，但改变 Latin 产品字幕输出
- **日期**：2026-07-30
- **状态**：已解决，优化被拒绝（feat-06806 / ADR-0028）。
- **现象**：Rec batch 1/2/4/6 sweep 中，batch=1 在双行微基准更快；若只看局部耗时，
  它会成为候选默认。但真实 Latin 产品源的最终 SRT SHA256 与冻结 Python Oracle 不再
  一致。
- **排查路径**：
  1. 先用固定 crop 验证 batch=1 与 batch=N 的 tensor、token、text 与置信度门。
  2. 再运行真实产品 CLI 和多源质量门，而不是用算子微基准替代端到端结果。
  3. 将逐源输出 SHA 纳入性能候选的 fail-closed 条件，确认漂移只在该执行策略下出现。
- **根本原因**：局部算子输出的容差通过不等于最终产品语义 exact；batch 形状与执行顺序
  可以改变推理尾数，尾数再经过 CTC/confidence/filter/跨帧选择后可能跨越离散决策边界。
- **解决方案**：产品 Rec batch 保持 6；新增 `source_output_sha256_exact` 硬门。任何
  提升微基准但改变任一来源最终 SRT hash 的线程、batch 或执行策略都不得进入默认配置。
- **验证结果**：batch=6 的 120s canonical C++ wall 仍比 Python 快 10.44%，性能后
  3 来源/614.272s 的 SRT SHA 和全部质量指标继续 exact。
- **相关文件**：`cpp/include/sublift/paddle.hpp`、
  `cpp/src/paddle/paddle_ocr.cpp`、`scripts/parity/check_paddle_perf.py`、
  `scripts/parity/check_paddle_gate.py`、`docs/DECISIONS.md` ADR-0028。

---

### macOS ASan 在 OpenCV/TBB 退出期崩溃，阻断发布级 sanitizer 门
- **日期**：2026-07-29
- **状态**：未解决；已完成最小隔离与可复现对照（feat-06607）。
- **现象**：`SUBLIFT_SANITIZE=ON` 的 `sublift_worker --version` 即使不启动 IPC、Pipeline 或
  视频处理，也会在输出版本后以 134 退出。栈落在 Homebrew TBB 2023.1.0 的
  `tbb::detail::r1::__TBB_InitOnce` 析构；关闭 LeakSanitizer 不改变结果。
- **排查路径**：
  1. 确认常规 Debug/Release Worker 退出正常，且 `--version` 在 `main` 中直接 return。
  2. 确认动态链为 Worker → OpenCV 4.14 → TBB 2023.1.0；ASan 只作用于本项目 target。
  3. 修复 OpenCV OFF 时 `Pipeline` 源码被排除、Worker 仍引用它的链接缺陷，产出不宣告
     engine/capability、明确拒绝作业的诊断 Worker。
  4. 用同一 ASan 配置比较：无 OpenCV/TBB 的诊断 Worker `--version` exit 0；重新链接
     OpenCV/TBB 后同命令稳定 exit 134。
- **根本原因**：已确认崩溃依赖 OpenCV/TBB 加载后的退出清理链；TBB 的
  `release_resources()` 通过 `destroy_system_topology_ptr` 间接跳转到无效地址。尚不能断言
  是 TBB 版本、macOS/AppleClang 组合，还是依赖初始化状态交互中的具体哪一项。
- **当前解决方案**：修复无 OpenCV 配置，使其可作为 sanitizer 诊断基线；产品 Worker 仍要求
  OpenCV，绝不将诊断构建作为回退或发布豁免。
- **后续方案**：在发布 runner 以相同 AppleClang 重建 OpenCV/TBB（优先验证 `WITH_TBB=OFF`
  或兼容 TBB），分别复跑 ASan；只有 OpenCV 产品路径的 sanitizer 完整通过才关闭此门。
- **相关文件**：`cpp/src/worker/CMakeLists.txt`、`cpp/src/worker/bridge_no_opencv.cpp`、
  `cpp/src/worker/engine_factory.cpp`、`docs/cpp/phase6.6-sanitizer-isolation.md`、
  `docs/reports/phase6-cpp-migration-quality-audit-2026-07-28.md`。

---

### ROI 后 producer/consumer 重叠未产生稳定的用户可见吞吐收益
- **日期**：2026-07-23
- **状态**：已归档，未采纳（feat-042）。
- **现象**：在 canonical Zootopia 1080p ROI、5fps、Vision、cjk 上，有界 Queue(maxsize=8)
  的 overlap 实现保持字幕 hash、固定 GT 质量、队列上限和取消/重启正确，却未达到预设的
  `end_to_end_wall ≤ serial × 95%`。两轮 warmup=1 + measured=3 的 median 比为 0.9559 和
  1.0587，后一轮反而更慢。
- **排查路径**：
  1. 确认 serial 与 overlap 的 detection hash 均为 `b2d35c1e25f156e1`，排除质量换速度。
  2. Queue high-watermark 达 8、backpressure 持续数秒，证明 producer 可领先但多数时间被
     单消费者反压。
  3. Phase 4 ROI 已将 frame materialize 显著压低，现有归因显示 OCR 是单消费者最大项；
     producer wall 包含满队列等待，不能当作可再隐藏的独立计算成本。
- **根本原因**：生产者—消费者只能重叠 extractor 与 consumer 的可并行部分。ROI 后 extractor
  足够快，Vision OCR、signature 与 changepoint 主导 end-to-end；线程/队列调度开销和 Vision
  时机抖动吞没了剩余收益。
- **解决方案**：回退到串行 path mode，不继续为阈值微调并发。feat-043 已确认 Vision 请求
  执行主导；下一步先扩充多源 GT，再以质量门评估代表帧排序与有效 OCR 调用，而非重开并发优化。
- **相关文件**：`docs/phases/phase4.1.json`、`debug/feat042/ab/`、`debug/feat042/ab_clean/`、
  `docs/plans/phase4.2-ocr-performance-attribution.md`。

---

### 显式 CJK cleanup 可能误删句首/句尾无空格英文
- **日期**：2026-07-12
- **状态**：待真实 GUI / 混排 GT 验证（当前不定级为 P1）
- **关联 feature**：feat-034 后续多语种与布局泛化验证
- **现象**：`cleanup_subtitle_text(..., script="cjk")` 为清理 Vision 合并进中文字幕行的稳定英文横幅，会删除与 CJK 首尾边界直接粘连的拉丁字符。字符串级复现：
  - `NPD动物警局` → `动物警局`
  - `苹果的iPhone` → `苹果的`
  - `欢迎来到ZPD` → `欢迎来到`
  - 带空格的 `NPD 动物警局`、`苹果的 iPhone` 会保留；中文内部的 `后来加入ZPD警局` 也会保留。
- **触发边界**：默认 `script="auto"` 不执行该删除；GUI 若从选中候选文字同时看到中文和英文，也会推断为 `auto`。风险发生在显式使用 `cjk`，或 Vision 将中英文拆成多个候选、用户只选择中文候选，导致 GUI 推断为 `cjk`，而后续全宽 OCR 又把句首/句尾英文识别进同一行时。
- **根本原因**：当前后处理以“拉丁字符是否直接粘在 CJK 边界”替代空间证据，无法区分背景横幅 `PHISON/SON` 与合法字幕内容 `NPD/iPhone/ZPD`。字符邻接关系本身不是可靠的噪声判据。
- **当前缓解**：无显式 profile 时默认 `auto`；GUI 按选中候选文字推断 `cjk/latin/auto`。该缓解降低了主路径误删概率，但不能覆盖 Vision 候选拆分或用户漏选英文候选的情况。
- **验证计划**：建立短时、确定性的多语种 GT 回归集，覆盖英文位于句首/句中/句尾、带空格/无空格、不同字幕位置；通过 GUI 真实选区与 path mode 记录实际 script 推断和最终文本。只有真实主路径复现后再升级优先级。
- **候选方案**：文字系统仅参与选行评分，不直接删除字符；扩展 `SubtitleProfile` 的水平范围，或使用 token/substring 级 bounding box，以用户选区几何和多帧空间稳定性区分字幕与横幅。完整方案实施前不得用品牌/缩写白名单代替。
- **相关文件**：`src/sublift/pipeline/line_select.py`（CJK 边界清理）、`apps/macos/Sources/SubLiftMac/Core/VideoCoordinateMapper.swift`（script 推断）、`tests/test_line_select.py`、`apps/macos/Tests/SubLiftMacTests/RegionGeometryTests.swift`

---

### dHash 对中文文本内容变化判别力不足，导致 timeline 漏分段
- **日期**：2026-07-03
- **状态**：部分缓解（feat-031b SSIM patrol 已实现，merged_into_neighbor FN 减少约 70%）
- **现象**：用 `scripts/run_timeline.py` 对 `debug/Zootopia_clip_hardsub1.mkv`（00:01:00~00:02:00 窗口）实测，段 33（00:01:47→00:01:59, 11600ms）合并了实际 4 个独立字幕段：
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
- **解决方案（feat-031b）**：引入 SSIM patrol 机制。在 STABLE 状态下，dHash 未触发时周期性比较锚帧与当前帧的二值化前景 mask SSIM。当 SSIM 显示结构变化明显时（< `ssim_patrol_threshold`）生成 CHANGE 候选，经稳定确认后触发 CHANGE 事件。patrol 与 dHash 候选共享稳定确认流程，但 trigger_reason 区分。
- **验证结果（feat-031d）**：在 Zootopia clip（1080p, 5fps, region_box 精准对齐）上对比：
  - baseline（patrol 关闭）：F1=65.0%, recall=60.9%, precision=69.7%, FN=47（全 merged）
  - optimized（patrol 启用）：F1=80.6%, recall=90.8%, precision=72.5%, FN=15（14 merged + 1 boundary）
  - merged_into_neighbor 减少 33 条（-70%），段 33 类合并漏检已消除
  - precision 不下降（+2.7pp），patrol 未引入新误检
- **参数落定**：F1 提升 +15.6pp ≥ 3pp 且 precision 不下降，但未达 95% 目标。patrol 记为推荐配置（`enable_ssim_patrol=True, interval=3, threshold=0.92`），默认保持关闭。
- **残留问题**：短字幕（<1.5s）召回率仍偏低（24%→60%），主要属于 IN/OUT 或采样不足，SSIM patrol 不能完全解决。
- **相关文件**：`src/sublift/pipeline/signature.py`（`compute_foreground_ssim`）、`src/sublift/pipeline/changepoint.py`（`_handle_patrol_path`）、`src/sublift/config.py`（patrol 配置）、`scripts/run_trace.py --compare-baseline`、`debug/reports/feat031_comparison.md`

---

### OCR 锚帧落在字幕过渡画面导致空文本
- **日期**：2026-07-03
- **状态**：部分解决（feat-033b：`ocr_anchor_delay_frames=2` + 多候选回退 + 默认保留空文本时间轴；仍有 text.empty 残留）
- **现象**：端到端 Pipeline 5fps 实测，段 5、段 11 出现 OCR 空文本。锚帧（IN/CHANGE 事件触发帧）恰好在字幕过渡画面（淡入/淡出/切换瞬间），Vision 未识别出文字，该段 text 为空。
- **排查路径**：
  1. 确认 OCR 引擎正常：同一视频其他段识别成功
  2. 确认锚帧选取逻辑：core.py 用 `anchor_frames[event.timestamp_ms]` 取事件触发帧，该帧是状态机确认变化的帧
  3. 分析根因：状态机用迟滞确认，事件 timestamp_ms 回溯到信号首次出现帧，但该帧可能正是字幕过渡帧
- **根本原因**：锚帧选取策略只考虑"变化首次出现"，未考虑该帧是否是"字幕稳定可读帧"。
- **解决方案（feat-033b）**：IN/CHANGE 后延迟 N 帧锁定主 OCR 锚；失败时回退稳定帧/段首/闭合帧；`drop_empty_text=False` 避免 timing 被抹掉。
- **残留**：部分段仍 `text.empty`（区域/水印/不可读），属 OCR 质量后置。
- **相关文件**：`src/sublift/pipeline/core.py`、`src/sublift/config.py`

---

### Pipeline 端到端 5fps 实测基线（2026-07-03）
- **测试条件**：`debug/Zootopia_clip_hardsub1.mkv`，00:01:00~00:02:00 窗口，5fps 采样，VisionOcrEngine（zh-Hans+en-US）
- **结果**：23 个实际字幕段，正确识别 21 段，**召回率/精确率 91.3%**，**0 误检**
- **缺陷分布**：
  - 2 段 OCR 空文本（段 5、11）：锚帧落在过渡画面（见上条 HURDLE）
  - 1 段合并 4 段（段 22）：dHash 对相似中文文本判别力不足（见首条 HURDLE）
- **结论**：Pipeline 端到端在 5fps 下表现扎实，打轴状态机很干净（0 误检）。主要瓶颈在 OCR 精度（空文本、英文水印干扰）和长段合并（dHash 对相似内容的区分度不够）。对 Phase 1 MVP 是不错的起点。
- **相关文件**：`scripts/run_timeline.py`、`debug/timeline_compare_result.txt`

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

---

### Swift MsgPack 库在嵌套 array of map 解码时崩溃
- **日期**：2026-07-05
- **状态**：已绕过（feat-015 决定继续用 JSON，见 ADR-0007d）
- **关联 feature**：feat-015（IPC 协议 + MsgPack 序列化）
- **现象**：feat-015 尝试把 IPC 序列化层从 JSON 换成 MsgPack。引入 Swift MsgPack 库后，单语言 roundtrip 测试全过，但跨语言测试（Python `msgpack.packb` → Swift decode）暴露两个库都有 bug：
  - `nnabeyang/swift-msgpack` 1.2.0：解码 `entries` 消息（`array of map` 嵌套结构）时 `MsgPackDecoder.swift:362` fatal error（`Unexpectedly found nil while unwrapping an Optional value`）；解码 `start_job` 的 `confidence_threshold` 字段（fixstr(20) key）时 key 被误读为整数 116。
  - `fumoboy007/msgpack-swift` 2.0.6：解码 `entries` 消息（`array of Codable struct`）时报 `extraBytesAtEndOfMessage(18)`，array 内的 map 解码后多出 18 字节未消费。
- **排查路径**：
  1. `nnabeyang/swift-msgpack`：先发现 `decodeToDict`（AnyCodable 路径）对嵌套 array 崩溃；切到 Codable struct 路径同样失败（`DecodingError.keyNotFound: Key 'confidence' not found`，`confidence` 是 10 字符 fixstr key 在嵌套 map 里丢失）
  2. 用独立 SwiftPM 工程复现，确认非工程配置问题，是库本身的解码器 bug
  3. 换 `fumoboy007/msgpack-swift`（Codable-based、有 msgpack-c 参考实现对比测试、声称 spec compliant），单层 map 解码正常，但嵌套 `array of Codable struct` 仍失败
  4. 单独解码 `SubtitleItem`（单层 map）成功，确认问题在 `array of struct` 这一层
- **根本原因**：两个 Swift MsgPack 库的 Codable 解码器在处理「map 内嵌 array 内嵌 map」结构时都有 bug。Python `msgpack` 生成的字节是标准合规的（`msgpack.unpackb` 能正确解码），但 Swift 端解码器在嵌套容器回溯时偏移了读指针。
- **解决方案**：feat-015 暂不引入 MsgPack，继续用 JSON。JSON 跨语言兼容性有保证，且：
  - 控制消息（start_job/progress/done）消息体小，JSON 开销可忽略
  - `frame` 消息的 `jpeg_bytes` 用 base64 编码膨胀 33%，但 5 fps 下吞吐量可接受
  - `entries` 消息批量推字幕，JSON 完全够用
- **后续**：ADR-0007c 修订版撤销；ADR-0007d 记录「继续用 JSON」决策。如未来需要 MsgPack，可考虑手写编解码（针对 7 类消息有限 schema，比通用库可控）。
- **教训**：**跨语言兼容性测试不可省略**。单语言 roundtrip 测试会掩盖库 bug——Swift encode → Swift decode 用相同的（可能有 bug 的）编码/解码逻辑，两边对称错误互相抵消。只有用 Python 生成的字节喂给 Swift 解码，才能暴露解码器的不对称 bug。
- **相关文件**：`docs/DECISIONS.md`（ADR-0007c 修订、ADR-0007d）、`docs/phases/phase2.json`（feat-015 evidence）

---

### feat-021 右侧字幕列表点击卡顿与双重高亮
- **日期**：2026-07-05
- **状态**：已修复（交互模型修正完成，feat-021 整体仍在进行中）
- **关联 feature**：feat-021（字幕时间轴 + 列表 + 编辑）
- **现象**：右侧字幕列表点击不跟手，点击与蓝色选中框出现之间有延迟；播放时出现浅蓝/深蓝两个高亮框不同步。
- **排查路径**：
  1. **10Hz 全列表重绘**：`PlayerModel.currentMs` 每 0.1s 发布，`SubtitleList` 直接 `@ObservedObject` 订阅 `PlayerModel` 会让右侧列表随播放时间整体 invalidation。修正为 `SubtitleList(editor:onSeek:)`，列表只接收 seek 回调，不再观察播放器。
  2. **双状态视觉混淆**：`selectedId`（编辑选中）与 `currentId`（播放位置）是两个概念。蓝色系统选中框只保留给 `selectedId`；`currentId` 改为行左侧灰色细条，移除 `play.circle.fill`，避免被误解为第二个播放按钮/蓝框。
  3. **点击后状态仍可能短暂不同步**：点击行时 `selectedId` 立即变蓝，但 AVPlayer seek 是异步的；旧的 periodic time observer 可能在 seek 完成前回写旧 `currentMs`，让播放位置细条短暂停在旧行。修正：选中变化时立即 `editor.updateCurrent(atMs: entry.startMs)`，`PlayerModel.seek` 增加 `pendingSeekMs`，seek 未完成前忽略旧 time observer，完成后再确认 `currentMs`。
  4. **点击区域不稳定**：`List` 默认 row inset 会让蓝色 selection 背景大于 SwiftUI 内容视图；同时文本上的双击编辑手势会吃掉部分单击。修正：行使用 `.listRowInsets(EdgeInsets())`，把 padding 放到 `SubtitleRow` 内部，让 row 内容视图覆盖整块蓝色区域；文本双击改为 `simultaneousGesture`，不阻断单击选中。
  5. **mkv 静态预览 seek 路径错误**：AVPlayer 失败进入 fallback 预览后，字幕列表点击仍调用普通 `seek`。修正：`ContentView` 在 `playerModel.loadFailed` 时走 `fallbackSeek(toMs:)`，同步更新静态预览时间点。
  6. **合并/拆分后状态悬空**：`merge(at:)` 保留第一条 `id`，若选中/当前播放命中被合并任一条则指向合并条目；`split(at:)` 保留原 `id` 给前半段；新增状态修正避免 `selectedId/currentId` 指向不存在条目。
- **当前实现**：`ContentView` 仍监听 `playerModel.currentMs`，但只调用 `editor.updateCurrent(atMs:)`；`SubtitleList` 只观察 `SubtitleEditor`。点击选中后立即同步当前播放细条，并通过 `onSeek(entry.startMs)` 跳转视频；整块 row selection 区域都参与点击命中。
- **验证**：`./init.sh` 通过；默认 `swift test` 在授权用户级 SwiftPM/Clang 缓存写入后通过（13 XCTest + 88 swift-testing）；`swift build -c release` 通过且当前无 warning。
- **相关文件**：`apps/macos/Sources/SubLiftMac/UI/SubtitleList.swift`、`apps/macos/Sources/SubLiftMac/UI/VideoPreview.swift`、`apps/macos/Sources/SubLiftMac/Core/SubtitleEditor.swift`、`apps/macos/Sources/SubLiftMac/App/SubLiftMacApp.swift`、`apps/macos/Tests/SubLiftMacTests/SubtitleEditorTests.swift`

---

### 普通 uv sync 修剪 Vision extra 导致 GUI OCR 启动失败
- **日期**：2026-07-05
- **状态**：已修复
- **关联 feature**：Phase 2 macOS GUI（Vision 默认引擎）
- **现象**：`swift build -c release && .build/release/SubLiftMac` 后，点击 Vision OCR 路径报错：`RuntimeError: Apple Vision 不可用... No module named 'objc'`。
- **排查路径**：
  1. `pyproject.toml` 中 `pyobjc-framework-Vision/Quartz` 属于 optional dependency group `vision`。
  2. 旧版 `init.sh` 运行普通 `uv sync`；uv 会让 `.venv` 精确匹配当前依赖集，未带 extra 时可能卸载/修剪 `pyobjc-*`。
  3. 只把同步阶段改成 `uv sync --extra vision` 还不够；后续 `uv run ...` 若不带 `--extra vision`，仍可能按默认依赖集重新同步并修剪 optional extra。
  4. 当前 `.venv` 一度出现 `pyobjc_core` dist-info 存在但 `site-packages/objc` 目录缺失的半残状态，需强制重装 `pyobjc-core` 修复。
- **根本原因**：GUI 默认使用 Vision 引擎，但标准初始化路径未固定 `vision` extra；uv 的精确同步语义会移除未声明在当前 extra 集中的包。
- **解决方案**：`init.sh` 中依赖同步和所有 `uv run` 验证命令统一带 `--extra vision`；并执行 `uv sync --extra vision --reinstall-package pyobjc-core` 修复本地半残安装。
- **验证**：`./init.sh` 通过；`.venv/bin/python -c 'import objc, Vision, Quartz'` 通过，随后再次运行 `./init.sh` 后直接导入仍通过。
- **相关文件**：`init.sh`、`pyproject.toml`

---

### SSIM patrol 过切分与短字幕漏检（feat-031 残留）
- **日期**：2026-07-06
- **状态**：部分解决（feat-033 residual：hysteresis=1 + 锚帧延迟 + 保留空文本；timing_f1 达 95.2%。仍有 ~6 条 merged 与 #15 单字 no_overlap）
- **关联 feature**：feat-031 / feat-033
- **现象**：patrol 默认开启后（优化 3），整体 F1 从 79.5% 提升到 86.7%，precision 91.1%，但暴露两个残留问题：

  **问题 1：patrol 过切分导致重复命中**
  7 组 GT 被切成两条检测段（gap=0ms，归一化文本因 OCR 噪声不等，dedupe 无法合并）：

  | GT | GT 文本 | det#1 | det#2 | 切分原因 |
  |---|---|---|---|---|
  | #19 | 跟胡尼克，一位平凡的狐狸 | #12 (792ms) | #13 (792ms) | 英文水印干扰 |
  | #21 | 一起揭穿了杨咩咩市长的阴谋 | #14 (791ms) | #15 (1000ms) | 英文水印干扰 |
  | #46 | 在ZPD，搭档合作是成功的基石 | #39 (2208ms) | #40 (4208ms) | 标点"，"vs"。" |
  | #50 | 这个家伙一直在利用船坞 | #44 (2583ms) | #45 (584ms) | 英文水印干扰 |
  | #52 | 行动由霍队长和阿杜队长带领 | #47 (1792ms) | #48 (2000ms) | 英文水印尾缀 |
  | #55 | 留守中间的是斑宝兄弟 -斑宝 | #51 (1792ms) | #52 (1625ms) | 英文水印尾缀 |
  | #79 | 反正你们得离开，不能进来 | #70 (1625ms) | #71 (791ms) | 标点"。"vs"，" |

  根因：patrol 在同一句字幕内部触发了 CHANGE（二值化 mask 局部波动），产生的两段 OCR 文本因噪声/标点差异归一化后不等，`dedupe._merge_adjacent` 无法合并。

  **问题 2：短字幕/闪现字幕漏检**
  `<=1200ms` 的 GT 只命中 7/16（43.8%）。漏检列表：

  | GT# | 时长 | 文本 | FN 类型 |
  |---|---|---|---|
  | #2 | 1125ms | （前情提要…） | merged_into_neighbor |
  | #5 | 1000ms | （前情蹄要…） | merged_into_neighbor |
  | #15 | 1000ms | 砰 | no_overlap |
  | #16 | 875ms | 不寻常的拍档… | no_overlap |
  | #17 | 1125ms | （动物方城市新闻台） | no_overlap |
  | #26 | 1125ms | （动物方城市警察学校） | merged_into_neighbor |
  | #59 | 833ms | （哈，胡） | no_overlap |
  | #68 | 1167ms | 嘿 -来了 | merged_into_neighbor |
  | #69 | 1083ms | 哈啰 | no_overlap |

  根因：`hysteresis_frames=2`（400ms@5fps）的迟滞吃掉了短字幕大部分时长；单字字幕（"砰"）前景占比低，难以达到 `presence_threshold`。
- **排查路径**：
  1. 用 `scripts/run_trace.py --compare-baseline` 对比确认 patrol 将 merged_into_neighbor 从 47 降到 14，但引入了 7 组过切分
  2. 用 `scripts/scan_params.py` 扫描确认 `min_duration_ms` 对打轴无影响（只影响 dedupe），`hysteresis=1` 略提升短字幕（+4pp）但会引入 FP
  3. 7 组过切分中归一化文本全部不等（OCR 噪声/标点差异），dedupe 无法合并
- **根本原因**：
  - 过切分：patrol 阈值 `0.92` 对同一句字幕内部的二值化 mask 局部波动过于敏感；CHANGE 候选确认后产生两段，OCR 文本因噪声不等导致 dedupe 失效
  - 短字幕：采样率 5fps + 迟滞 2 帧 = 400ms 迟滞窗口，对 `<=1000ms` 的字幕吃掉了 40%+ 时长；单字字幕前景占比不足
- **feat-033 已落地**：
  1. **诊断**：纯打轴 trace 对 52–63s 新闻簇有 IN/CHANGE，最终 SRT 空洞主因是 **M1c（OCR 空/confidence 滤掉段）**，非状态机卡 EMPTY。
  2. **锚帧延迟 + 多候选 OCR**（`ocr_anchor_delay_frames=2`）+ **`drop_empty_text=False`**。
  3. **`hysteresis_frames=1`** 补短字幕。
  4. 验收：`baseline-no-filter` F1 91.2% → **95.2%**（R 83.9%→92.0%，P 100%→98.8%）。
- **仍开放**：merged residual（新闻簇 / 哈啰 等）、单字「砰」。
- **feat-034 已收敛 OCR 噪声**：P1 修复后固定 GT 达 usable 92.0%、CER macro 3.2%、noise 0、empty 0；timing F1 97.7%、precision 98.8%。版本化报告见 `benchmark/reports/quality-baseline.md`；原始历史产物见 `debug/benchmark-reports/feat034_p1_fix2/`。
- **相关文件**：`src/sublift/pipeline/core.py`、`dedupe.py`、`config.py`、`debug/reports/feat033_diagnosis.md`、`debug/benchmark-reports/feat033_final/`

---

### feat-033：打轴 residual 中「检出后被 OCR 抹掉」
- **日期**：2026-07-09
- **状态**：已解决（主路径 M1c）
- **关联 feature**：feat-033
- **现象**：`baseline-no-filter` 报告 8 条 `no_overlap`（含 52–63s 新闻簇），用户误判为状态机未重新 IN。
- **排查路径**：
  1. `run_trace.py` + MockOcr 从事件重建段 → 纯打轴 **0** no_overlap，新闻窗有完整 IN/CHANGE。
  2. 对比 Vision 最终 SRT：段被 confidence 置空 / empty-filter 丢弃 → 评测成 no_overlap。
- **根本原因**：OCR 锚帧落在过渡画面或识别失败时，后处理把**时间轴证据**一并删除。
- **解决方案**：延迟锁定 OCR 锚帧；失败时多候选重试；默认不丢弃空文本段。
- **相关文件**：`core.py`（`_open_segment` / `_close_segment` / `ocr_segment`）、`dedupe.py`（`drop_empty_text`）

---

### bridge 批量缓冲与 Vision 引擎内存泄漏导致长视频 OOM
- **日期**：2026-07-06
- **状态**：已解决（feat-029，已验证修复）
- **关联 feature**：feat-029（增量处理架构）
- **现象**：GUI 提取长视频时内存占用随帧数线性增长，2 分钟 1080p clip ~3.6GB，10 分钟 ~18GB，长视频必 OOM。
- **排查路径**：
  1. 审查 `pipeline/core.py` 的 `feed` 和 `bridge.py` 的增量抽取机制，发现 `path_mode` 下已经在用单线程边收帧边处理，并将每条产出的 `SubtitleEntry` 异步推入队列。
  2. 即使架构流式化后，长视频 4K 提取测试中，内存仍然线性增加至 2.1 GB（对应 ~300 帧裁剪图片）。
  3. `tracemalloc` 定位到 `PIL.Image.frombytes` 分配了内存。
  4. 引入 `gc.get_referrers` 调查，发现 Python 层存活 Image 数量已归零。
  5. 锁定到 `vision.py` 中的 PyObjC 桥接：`CGDataProviderCreateWithData` 调用接收了 Python 分配的二进制图片 `bytes` 缓冲，但未提供释放回调，导致 `bytes` 在 Objective-C 桥接层保留引用，随帧数无限泄漏。另外，缺乏 `objc.autorelease_pool`，其他 CF 对象如 `CGImage` 也会堆积到全局自动释放池中直到线程退出。
- **根本原因**：
  1. 历史版本的 bridge 把本可流式的 Pipeline 退化成批量缓冲模式。
  2. Apple Vision 引擎桥接代码缺乏 `autorelease_pool` 保护。
  3. PyObjC 将 Python 二进制数据转交给 CoreFoundation 时缺乏显式生命周期管理。
- **解决方案**：
  1. Pipeline 已由 `run_frames` 退化为彻底基于 `feed` / `ocr_segment` 驱动的流式接口。`bridge.py` 的 `_worker` 直接逐帧驱动并在段闭合时及时 push。
  2. `vision.py` 的 `recognize` 中加入 `with objc.autorelease_pool():` 上下文。
  3. 废弃 `CGDataProviderCreateWithData`，改为通过 `NSData.dataWithBytes_length_` 包转后调用 `CGDataProviderCreateWithCFData(ns_data)`，将底层二进制的生命周期安全委托给 Toll-Free Bridging 体系。
- **验证结果**：4K 视频测试，内存消耗峰值从 2157 MiB 下降到极低 (Top 3 分配仅为数十 KB)，未见任何线性增长；后续 feat-030 自动审计首条反馈 0.68s、取消响应 0.108s，ffmpeg PID 退出且第二任务可正常启动；核心 Benchmark 无回退。≥10 分钟非 Zootopia 视频的 GUI 拖拽、进度观感、取消与重启手工验收由用户决定暂缓，仍是体验覆盖风险，不影响已验证的资源泄漏修复结论。
- **相关文件**：`src/sublift/pipeline/core.py`（`feed` 驱动）、`src/sublift/ipc/bridge.py`（流式推送）、`src/sublift/ocr/vision.py`（内存防泄漏修复）
