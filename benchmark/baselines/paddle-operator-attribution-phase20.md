# Paddle OCR 算子优化基准与等价性（Phase 20）

> **状态：** Phase 20 D04 收口证据（2026-10-22，分支 `opt/paddle-operators`）
> **基线：** `main` 的 `f224117`（优化前）vs 同分支工作树（优化后）
> **协议：** 同机、同负载、Release（`build/cpp-rel`，OpenCV 4.14 / ORT 1.28.0）；
> `SUBLIFT_STAGE_TIMING=1`，交错运行 baseline 与 optimized；SRT sha256 作为硬等价门。
> **测量环境警告：** 本机为共享开发机，运行期间系统 load 1.9-7.7，同一负载的 core wall
> 波动可达 1.7 倍。绝对值只用于同批次对比；百分比以阶段中位数与硬等价证据为准。

## 1. 目的与范围

本报告回答三件事：

1. Paddle 适配器内 **C++ 算子**（预处理、DB 后处理、CTC 解码、张量装配）在 Phase 20
   优化前后的成本变化；
2. 这些优化的**逐字节等价性**（SRT sha256 / 张量单测）；
3. ORT 推理配置（intra_op 线程、CPU arena）是否有可用杠杆（结论：没有，维持冻结值）。

固定负载：Zootopia 1920x1080、254.272s、5fps、paddle（PP-OCRv6-small）、cjk、默认
0.6MP det 几何。附加两个派生片段用于降低测量时长：前 30s（字幕稀疏，64 次 det / 21 段）
与 20-80s（字幕密集，18 条字幕）。

## 2. 阶段成本：基线

30s 片段的 baseline 阶段中位数（profiler 子代理独立复测，低负载稳定子集 m4-m6；
`debug/phase20/profiler-report.md`）：

| stage | calls | median ms | 占 core |
|---|---:|---:|---:|
| `paddle.det_infer` | 64 | 4278.8 | 65.0% |
| `paddle.rec_infer` | 35 | 643.4 | 9.8% |
| `signature` | 150 | 599.5 | 9.1% |
| `frame_fetch` | 151 | 392.3 | 6.0% |
| `changepoint` | 150 | 282.6 | 4.3% |
| `paddle.det_preprocess` | 64 | 67.5 | 1.0% |
| `paddle.crop` | 35 | 63.9 | 1.0% |
| `paddle.det_postprocess` | 64 | 32.2 | 0.5% |
| `paddle.cls_infer` | 35 | 43.3 | 0.7% |
| Paddle 标量算子合计 | - | 179.3 | 2.5-2.6% |

全片 254s 的 baseline（单次，`full_base_timing.log`）：core_wall 38608.3ms；
det_infer 19745.8、rec_infer 6015.8、det_preprocess 281.4、crop 298.3、det_postprocess 115.0。

## 3. 优化后的阶段对比

### 3.1 30s 稀疏片段（交错 3 组，baseline 与 optimized 各 3 次）

| stage | baseline median | optimized median | delta |
|---|---:|---:|---:|
| core_wall | 6549.9 | 6555.7 | +5.8（噪声内） |
| `paddle.det_preprocess` | 68.4 | 43.3 | **-25.1（-37%）** |
| `paddle.det_postprocess` | 31.1 | 24.8 | **-6.3（-20%）** |
| `paddle.crop` | 63.0 | 63.1 | 0（未改） |
| `paddle.rec_decode` | 未计时（残差 <=11.7 合计） | 2.7 | 估计 -8~-10 |

算子侧合计约 -35~-40ms ≈ core wall **-0.6%**；端到端中位数差异被 ORT 推理波动吞没。

### 3.2 60s 字幕密集片段（交错 3 组）

| stage | baseline median | optimized median | delta |
|---|---:|---:|---:|
| core_wall | 8490.1 | 8449.2 | -40.9 |
| `paddle.det_preprocess` | 47.7 | 28.5 | **-19.1（-40%）** |
| `paddle.det_postprocess` | 19.6 | 14.1 | **-5.5（-28%）** |
| `paddle.rec_decode` | 108.3（关闭 SIMD 的 A/B） | 6.8（NEON） | **-101.4（-94%）** |
| `paddle.crop` | 45.6 | 44.4 | 0（未改） |

算子侧合计约 -126ms ≈ core wall **-1.5%**（端到端中位数 -0.5%，其余为 ORT 推理波动）。

### 3.3 全片 254s（单次 A/B，用于真实内容口径）

| stage | baseline | optimized | delta |
|---|---:|---:|---:|
| core_wall | 38608.3 | 38541.8 | -66.5（噪声内） |
| `paddle.det_preprocess`（296 calls） | 281.4 | 168.8 | **-112.6（-40%）** |
| `paddle.det_postprocess`（296 calls） | 115.0 | 87.7 | **-27.2（-24%）** |
| `paddle.rec_decode`（257 calls） | 未计时（残差约 121.6） | 24.8 | 估计 -115 |
| `paddle.crop`（254 calls） | 298.3 | 287.9 | -10.4（噪声） |

- 未计时残差法：baseline `ocr` 26980.7 - 已计时 paddle 阶段 26859.2 = **121.6ms**；
  optimized `ocr` 26842.4 - 阶段合计（含 rec_decode 24.8）= **6.8ms**。两者差值约 115ms
  即 scalar CTC 解码相对 NEON 的节省（同口径装配/分配）。
- 全片算子侧合计约 **-255ms ≈ core wall -0.66%**；端到端 wall 因 det_infer 单次波动
  （+100ms）无法在单次运行中显现。

### 3.4 结论

- 优化收益与**字幕密度**强相关：稀疏片段约 -0.6%，字幕密集内容约 -1.5%，全片真实内容
  约 -0.66%。算子侧结构天花板约 2.5-3%（ORT 推理占 75%）。
- `det_preprocess` 稳定下降约 40%，`det_postprocess` 稳定下降 20-28%，CTC 解码在
  密集内容上下降约 94%。

## 4. ORT 推理配置评估（测量，不采纳）

在 60s 密集片段上，用临时环境开关复测 intra_op 线程与 CPU arena（默认冻结值：4 线程、
arena off；每个配置 2 次）：

| 配置 | core_wall (ms) | det_infer (ms) | rec_infer (ms) | SRT sha256 |
|---|---|---|---|---|
| default（intra_op=4, arena off） | 8472 / 8467 | 3408 / 3438 | 1372 / 1394 | f479e8ed... |
| intra_op=0（ORT default=10） | 9060 / 9060 | 3621 / 3652 | 1683 / 1679 | f479e8ed... |
| intra_op=8 | 9797 / 9622 | 3454 / 3410 | 2276 / 2187 | f479e8ed... |
| intra_op=10 | 11220 / 11080 | 4188 / 3939 | 2846 / 2938 | f479e8ed... |
| arena on | 8446 / 8499 | 3435 / 3457 | 1393 / 1395 | f479e8ed... |

结论：**intra_op=4 已是本机最优**；增加线程显著拖慢 rec 推理（小 shape 的线程调度/争用），
arena 开关无稳定收益。所有配置的输出 SRT 相同，说明线程配置不改变本模型的可观察结果；
但按 ADR-0025/0028 冻结项，维持默认值与 arena off，不引入配置面。

## 5. 等价性验证

| 项 | 证据 |
|---|---|
| 全片默认 0.6MP | `f77296e3736be0f09d3752eedb7cc2e5f3f9ceaef0a15eb6936f160d4854fce7`（baseline = optimized） |
| 全片 `--det-pixel-budget 0` 回滚 | `56b1b08acdf2770addf65d20c5e8ff02947f7125a759bc62ec06521889e9412e`（与 Phase 18 D04 一致） |
| 30s 片段 | `6b047aa8813c182bc7730f949d43cace2b2dd8d08722c40b2c9b83c4d68e520b` |
| 60s 密集片段 | `f479e8ed7172b62debd5f4887d8f2d2d1f28cb3abbc3fe991807b34fc176926b` |
| 算子等价单测 | det 张量 memcmp、CTC argmax/decode、DB serial vs parallel、cls/rec sentinel、strided global preprocess（632 assertions / 36 test cases at `[paddle]` filter） |
| Debug CTest | 281/281 passed（`build/cpp`, 99.43s） |
| 产品门 | `./scripts/verify-product.sh`: 22 项 OK；唯一失败为 `swift test` 在 DSH 沙箱内无法执行 `sandbox-exec`（环境限制），Web/Server/CLI/CTest 全通过。另以 `--disable-sandbox` 复跑：143 个 swift-testing 用例与 UI/集成 XCTest 通过；Phase8 真实 IPC 闭环在整套运行中超时、单测重跑通过（2.7s，时序/环境 flake，改动不涉及 Swift/Worker） |

## 6. 复现命令

```bash
# 阶段归因（计时开关默认关闭，产品零成本）
SUBLIFT_STAGE_TIMING=1 ./build/cpp-rel/bin/sublift extract debug/Zootopia_clip_1080p.mp4 \
  --engine paddle --fps 5 --script cjk --confidence 0 -o /tmp/out.srt

# 等价性
sha256sum /tmp/out.srt   # 期望 f77296e3...
./build/cpp-rel/bin/sublift extract debug/Zootopia_clip_1080p.mp4 --engine paddle --fps 5 \
  --script cjk --confidence 0 --det-pixel-budget 0 -o /tmp/retry736.srt   # 56b1b08a...

# 算子单测
./build/cpp-rel/bin/sublift_tests "[paddle]"
ctest --test-dir build/cpp --output-on-failure
```

## 7. 证据索引

- 审计与优化设计：[`docs/design/paddle-operator-audit.md`](../../docs/design/paddle-operator-audit.md)
- 本地原始数据（gitignored）：`debug/phase20/{operator-audit.md,profiler-report.md,measure/,final_*,full_*,ort_*}`
- Phase 20 详情：[`docs/phases/phase20.json`](../../docs/phases/phase20.json)
