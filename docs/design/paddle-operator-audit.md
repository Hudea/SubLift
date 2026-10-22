# Paddle OCR 算子审计与优化（Phase 20）

> **状态：** Phase 20 D01 审计基线 + D02/D03 实施记录（2026-10-22，分支 `opt/paddle-operators`）
> **基线代码：** commit `f224117`（本 Phase 修改前）；文中 `file:line` 均按该基线版本标注，
> 修改后的行号请按函数名锚定。
> **审计对象：** `cpp/src/adapters/paddle/` 下从 RapidOCR 3.9.2 Python 包迁移而来的
> Paddle OCR 预处理 / 后处理 / 解码算子，以及 `paddle_ocr.cpp` 的推理编排。
> **配套测量：** [benchmark/baselines/paddle-operator-attribution-phase20.md](../../benchmark/baselines/paddle-operator-attribution-phase20.md)。

---

## 1. 结论摘要

1. **成本结构**：固定 1080p / 5fps 负载中，ORT 推理占 core wall 约 **75%**
   （其中 `paddle.det_infer` 单项约 **65%**）；Paddle 适配器内可优化的 C++ 标量算子
   （det/global/crop/cls/rec 前处理 + DB 后处理）合计约 **2.5%**；未计时残差（CTC 解码 +
   结果装配 + 批张量分配）在稀疏素材上上限约 0.16%。**算子侧的整体天花板约 2.5–3%**，
   本 Phase 不能、也没有承诺端到端大幅提速。
2. **本 Phase 落地的优化**（全部逐位等价）：
   - det 前处理：复用 engine 级 NCHW 缓冲（去掉每次 7.37MB 值初始化）+ 按行并行
     归一化/NCHW 转换 -> 全片 296 次调用 **281.4 -> 168.8ms（-40%）**。
   - DB 后处理：`cv::threshold`+`convertTo` 双 pass 改为一次 `cv::compare`；膨胀核静态化；
     候选框评估按轮廓并行、结果按原顺序压缩 -> 全片 **115.0 -> 87.7ms（-24%）**。
   - CTC greedy decode：argmax 改为 NEON 8 lane 向量化（保持"首个最大值、NaN 永不胜出"
     标量语义）-> 字幕密集素材上解码约 **108 -> 6.9ms（-94%）**；全片估计省约 115ms。
   - cls/rec 批输入改为免值初始化分配（消除与 `prepare_padded_tensor` 重复的整体清零）。
   - 修正审计发现的 P1 正确性缺陷：`prepare_global_image` 在无需 resize/pad 时按 packed
     连续读，`stride > width*3` 会逐行错位；现按行拷贝（含回归测试）。
3. **经测量后拒绝的优化**（记录以防重提）：
   - **crop 跨 box 并行**：真实负载中每次 recognize 仅 1–2 个 box，外层并行被
     `warpPerspective` 内部并行与调度开销抵消；A/B 显示中性或略差，已回退。
   - **CTC SIMD 的稀疏素材收益被高估**：30s 稀疏片段整体未计时残差仅约 12ms；
     SIMD 的价值在字幕密集内容（真实产品负载）而非检测为空占多数的片段。
   - **ORT 线程/arena 调参**：intra_op=4 已是本机最优；8/10 线程与 default(10) 均显著更慢
     （rec 推理约 +60%~110%），arena 开关无稳定差异；输出 SRT hash 不变。冻结值维持不变。
4. **迁移语义**：逐项对照 RapidOCR 3.9.2 后，global/det/DB/crop/cls/rec/CTC 主路径一致，
   未发现影响 SRT 文本、box、confidence 的偏差；仅存在可忽略的边界差异（见第 3 节）。

---

## 2. 成本结构（基线 `f224117`，Release，`SUBLIFT_STAGE_TIMING=1`）

固定负载：Zootopia 1080p、5fps、paddle、cjk、默认 0.6MP det 几何。

| stage | calls | 30s 片段 (ms) | 全片 254s (ms) | 类型 |
|---|---:|---:|---:|---|
| `paddle.det_infer` | 64 / 296 | 4643.45 | 19745.8 | ORT（冻结边界） |
| `paddle.rec_infer` | 35 / 257 | 692.88 | 6015.8 | ORT（冻结边界） |
| `paddle.cls_infer` | 35 / 257 | 43.66 | 344.3 | ORT（冻结边界） |
| `paddle.det_preprocess` | 64 / 296 | 71.72 | 281.4 | C++ 标量（已优化） |
| `paddle.crop` | 35 / 254 | 58.89 | 298.3 | OpenCV warp（保持） |
| `paddle.det_postprocess` | 64 / 296 | 42.37 | 115.0 | C++ 标量（已优化） |
| `paddle.global_preprocess` | 64 / 296 | 4.53 | 16.0 | 整帧拷贝（保持） |
| `paddle.cls_preprocess` | 35 / 257 | 2.03 | 16.4 | 已去双重清零 |
| `paddle.rec_preprocess` | 35 / 257 | 2.37 | 27.0 | 已去双重清零 |
| 未计时残差（CTC + 装配 + 批分配） | - | <=11.7（稀疏） | ~121.6 | 已插桩/优化 |

> 30s 稀疏片段中 64 次 det 里有 29 次检出为空（pipeline 代表帧语义），因此 det 占比与
> 字幕密度无关，而 CTC/rec 成本随字幕密度显著上升；本 Phase 的收益在字幕密集内容更大。

---

## 3. RapidOCR 3.9.2 -> C++ 对照结论

审计报告（`debug/phase20/operator-audit.md`，基线行号）逐项比对了 Python 源码与 C++：

- **global preprocess**：max/min side 比例、32 倍数 half-even、垂直 padding 规则一致。
- **det preprocess**：几何策略（ADR-0042 0.6MP）、`cv2.resize` INTER_LINEAR、float32
  归一化序列、RGB->BGR + NCHW 均一致（LUT 复刻 NumPy 三个 ufunc 的 float32 边界）。
- **DB 后处理**：严格阈值、2x2 膨胀、findContours、minAreaRect 角点排序、fast/slow
  box score、pyclipper JT_ROUND 复刻（含 0.25 arc tolerance）、坐标 half-even 回映与
  sorted_boxes 的 positional score quirk 一致，且有零容差单测锁定。
- **crop / cls / rec**：warpPerspective 参数、rot90 条件、48x192 cls 左对齐零填充、
  rec `max(320/48, w/h)` 目标宽与右对齐零填充、OpenCV 11-bit 定点 resize 复刻均一致。
- **CTC**：blank/去重/字典偏移、每 token 五位舍入后取均值一致。
- **已知边界差异**（均无实际输出影响）：crop 尺寸 float vs float64 `hypot`；CTC confidence
  `round` half-away vs `np.round` half-even（1e-5 量级）；`max(2,...)` 防御性钳制；
  `aabbs` 在 `db_postprocess` 与 `sort_db_result` 中重复计算且产品路径不读取。
- **P1 正确性缺陷（已修）**：`ppocr_det_preprocess.cpp` 的 global preprocess 在无
  resize/pad 分支使用连续 `assign`；`stride > width*3` 时逐行错位。当前 bottom-crop 负载
  不触发，但 `mode=full + region_box` / 自动回退路径会触发。现改为按行 `memcpy`，并新增
  strided 输入回归测试。

---

## 4. 实施与等价性

### 4.1 已落地改动

| 项 | 位置 | 内容 |
|---|---|---|
| det 前处理缓冲复用 | `ppocr_det_preprocess.*`、`paddle_ocr.cpp` | 新增 `prepare_det_input_into()`；`PaddleOcrEngine::Impl` 持 grow-only 复用缓冲；归一化/NCHW 按行 `cv::parallel_for_`；删除每次 7.37MB 值初始化 |
| DB 后处理 | `ppocr_db_postprocess.cpp` | `cv::compare` 单 pass 出 8U bitmap；静态膨胀核；候选评估按轮廓并行（`DBPostProcessOptions::parallel` 为等价测试开关），结果按轮廓顺序压缩；serial 路径保留 |
| CTC argmax | `ppocr_ctc.hpp` | NEON 8 lane 扫描（块内先判无更大值则跳过；有则取块最大并定位首个等值 lane），标量参考实现保留；string/token reserve |
| cls/rec 输入缓冲 | `paddle_ocr.cpp` | `std::make_unique_for_overwrite<float[]>` 替换值初始化 vector，由 `prepare_padded_tensor` 负责填充 |
| global preprocess 越界读 | `ppocr_det_preprocess.cpp` | 按行拷贝 strided 源，修复错位缺陷 |
| 阶段计时 | `paddle_ocr.cpp`、`scripts/measure_native_attribution.py` | 新增 `paddle.rec_decode`、`paddle.ocr_post`、`paddle.cls_apply`，默认关闭、零成本 |

### 4.2 等价性证据

- 新增/扩展单测（`cpp/tests/paddle_*_test.cpp`）：
  - det 张量：优化实现 vs 测试内标量参考实现，5 组几何/尺寸随机图像 `memcmp` 逐位相等；
  - CTC argmax：随机、全等、单峰、并列、NaN、正负 inf、非 NEON 长度边界与标量参考逐位一致；
    完整 decode 的 text/confidence/token 与参考解码一致；
  - DB 后处理：随机概率图 + 随机矩形下 `parallel=false/true` 的 quads/score/aabbs 完全相等；
  - cls/rec 张量：sentinel 填充后调用，验证全部元素被定义且右侧 padding 为 0；
  - global preprocess：strided 输入无 resize/pad 时逐行内容正确。
- 真实视频：30s 片段 SRT sha256 `6b047aa8...`、60s 字幕密集片段 `f479e8ed...`、全片默认
  0.6MP `f77296e3...`、`--det-pixel-budget 0` 回滚 `56b1b08a...` 均与基线一致（详见基准报告）。
- Debug CTest 与 `./scripts/verify-product.sh` 结果见 Phase 20 evidence。

---

## 5. 剩余空间与后续建议

1. **ORT 推理内核（约 75%）不可在本 Phase 修改**：模型 SHA、ORT 二进制、intra_op=4、
   arena off 均为 ADR-0025/0028 冻结项；本 Phase 的线程/arena 复测确认默认值已最优
   （详见基准报告），不需要为此另立 ADR。
2. **det 调用次数是比算子更大的结构杠杆**：全片 296 次 det 仅对应 186 个 OCR segment
   （其中 30s 片段 64 次 det / 21 段），line-select 重试导致重复检测；若要显著提速，
   应另立 Phase 处理"代表帧/重试策略"，而不是继续压算子。
3. **pipeline 侧 C++ 算子**：`signature`（全片 5.4s，14.0%）与 `changepoint`（3.1s，8.1%）
   比 Paddle 算子侧大一个量级，但属 Phase 16 归因范围，不应塞入本 Phase。
4. **可继续尝试但收益有限**：det resize 与归一化融合（预计再省约 80-100ms/全片，需逐位
   验证 11-bit 定点路径）；`Ort::IoBinding` 预分配输出缓冲（rec 输出每 Run 数 MB，未实测）。
5. **验收口径提醒**：本机为共享环境，端到端 wall 波动可达 1.7 倍，70-150ms 级收益必须用
   阶段中位数 + 算子单测 + SRT sha256 证明；不要用单次端到端 A/B 声称百分比。
