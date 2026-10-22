# Det 像素预算：探索交付报告

> **状态**：探索完成并已提交（2026-10-21），分支 `exp/det-pixel-budget`，基于
> `opt/native-perf-acceleration` @ `e8561c9`。**未合入、未改产品默认值。**
> **证据主文档**：[`det-input-geometry.md`](det-input-geometry.md)（实验方法、全量 CER 与
> fixture 结果）；本报告只记录本次实现、测试与固定负载复核。

## 1. 结论

1. Det 占了 Native Paddle 产品路径 **84.35%** 的 core wall，根因不是模型慢，而是
   `limit_side_len=736, limit_type=min` 把 1920×324 的 bottom-crop 放大到
   **4352×736（3.203MP）**：成本与源分辨率无关，永远按 3.2MP 付。
2. 改为**像素预算 0.7MP**（仅在面积不足时上采样，不缩小）后，同一固定负载 det 输入变为
   **2048×352（0.721MP，像素少 4.44×）**，端到端 **120.26s → 41.50s（2.90×）**。
3. 质量：按项目官方 one-to-one 诊断（temporal IoU 0.5），文本质量显著改善：1080p CER
   macro 17.27% → 3.67%（exact 72.6% → 92.0%），720p 13.18% → 4.83%，480p 8.78% → 3.13%，
   `mixed_multiline` 55.12% → 7.50%，`latin_motion` 37.62% → 0.00%。但 720p/480p 的
   timing precision 各降约 1pp，1080p/720p split 数增加，**不能概括为“质量全面不降”**。
   旧脚本的 1080p matched CER 25.97% → 13.15% 只作策略筛选，见 3.3 与 3.4。
   Zootopia 三档是同源下采样，独立内容源只有 Zootopia、mixed、latin 三个。
4. 本次把几何策略从内联 env 判断收敛为**纯函数接口 + 显式重载**，并补齐单测；
   默认行为逐位不变（固定负载 SRT sha256 仍为 `56b1b08a…`）。**默认切换仍需独立 ADR 与
   parity / golden 重冻，未在本分支实施。**

## 2. 交付内容

| 文件 | 变更 |
|---|---|
| `cpp/src/adapters/paddle/ppocr_det_preprocess.hpp` | 新增 `DetInputGeometry`、`DetInputSize`、`det_input_geometry_from_env()`、`det_input_size()`；新增 4 参 `prepare_det_input()` 重载 |
| `cpp/src/adapters/paddle/ppocr_det_preprocess.cpp` | 几何计算抽成纯函数；3 参入口保持读 env（`SUBLIFT_DET_MIN_SIDE` / `SUBLIFT_DET_PIXEL_BUDGET`，2026-10-22 起仅诊断）；4 参入口接受显式策略 |
| `cpp/tests/paddle_det_preprocess_test.cpp` | 新增默认 736、0.7MP 预算、预算小于原生、纯 native、显式策略预处理 5 个用例 |
| `docs/design/det-input-geometry.md` | 更新状态与建议段，标注实现已落地、默认未切换 |
| `docs/design/det-pixel-budget-delivery.md` | 本报告 |

语义边界：

- `pixel_budget > 0` 时优先于 `min_side`；仅当 `width*height < budget` 时按面积比开方放大，
  **永不下采样**。
- 输出边长仍按 RapidOCR 规则取 32 的倍数（半偶舍入），与既有 736 路径同一个取整函数。
- 无效或非正的 env 值回退默认（736 / 关闭预算）。
- 默认构造 `DetInputGeometry{}` = `{min_side=736, pixel_budget=0}`，即产品现状。

## 3. 验证

### 3.1 单元与全量测试

```bash
./build/cpp-rel/bin/sublift_tests "[paddle][det][preprocess]"
# All tests passed (29 assertions in 5 test cases)

ctest --test-dir build/cpp-rel --output-on-failure
# 100% tests passed, 264/264
```

新增断言覆盖：

- 1920×324 → `4352×736`（默认 736）、`2048×352`（0.7MP）、`1920×320`（预算小于原生）；
- 960×256 → `2752×736`（默认）、`1632×448`（0.7MP）；
- `{min_side=0, pixel_budget=0}` → `1920×320`（纯 native，不放大且保持 32 倍数）；
- 显式 0.7MP 预处理输出的宽高与 NCHW 大小（`3*1632*448`）。

### 3.2 固定负载 A/B（无计时，本分支实测）

| 项 | 值 |
|---|---|
| 负载 | `debug/Zootopia_clip_1080p.mp4`，1920×1080，254.272s |
| 参数 | `--engine paddle --fps 5 --script cjk`，bottom-crop 1920×324 |
| 构建 | `build/cpp-rel`，Release，macOS arm64，OpenCV 4.14 / ORT 1.28.0 |
| 协议 | 同机顺序执行，各 1 次；预算路径与实验分支同配置 |

| 策略 | det 输入 | wall | 字幕条数 | SRT sha256 |
|---|---|---:|---:|---|
| 默认 736 | 4352×736（3.203MP） | 120.26s | 81 | `56b1b08acdf2770a…` |
| 预算 0.7MP | 2048×352（0.721MP） | **41.50s** | 83 | `e667beef1110af7b…` |

- 默认输出与冻结基线**逐位一致**，证明接口重构没有改变产品行为。
- 预算输出哈希与实验分支 1080p/0.7MP 的复跑结果完全一致（`e667beef1110af7b…`），
  所以两次运行的输出是同一份字节；但“质量结论”必须用官方诊断重算（3.4），不能因为
  hash 相同就直接引用旧口径数字。
- 2.90× 端到端与成本模型吻合：`det_infer` 基线 103.0s × (0.721/3.203) ≈ 23.2s，
  加上其余约 19s 非 det 成本，合计约 42s，与实测 41.50s 吻合。

### 3.3 成本归因复核（计时开，反向顺序）

先跑 0.7MP、再跑 736，避免“先冷后热”偏向；两次输出哈希仍与 3.2 相同。

| 项 | 736 | 0.7MP | 变化 |
|---|---:|---:|---:|
| wall | 150.42s | 48.34s | **3.11×** |
| core_wall | 149,750ms | 47,687ms | −102,063ms |
| `ocr` calls | 186 | 186 | 0 |
| `paddle.det_infer` calls | 292 | 287 | −5 |
| `paddle.det_infer` total | 127,576ms | 26,477ms | **−101,099ms** |
| `paddle.det_infer` / call | 436.9ms | 92.3ms | **4.74×** |
| `paddle.rec_infer` calls | 190 | 261 | +71 |
| `paddle.rec_infer` total | 5,707ms | 6,759ms | +1,052ms |

det 省下的 101.1s 占 core wall 差异的 **99.1%**；OCR segment 调用数完全相同，Rec 调用反而
多了 37%。因此加速来自 det 输入像素下降，不是“少跑 OCR”或换引擎。计时开关本身带来约
25%/20% 的绝对开销，所以 3.3 的绝对值高于 3.2，但方向和哈希一致。

### 3.4 官方质量复核

用 `sublift.benchmark.diagnostics.analyze_entries`（one-to-one、temporal IoU 0.5）对实验
产出的 SRT 重新评分，结果如下（完整表见
[`det-input-geometry.md`](det-input-geometry.md) 4.4）：

| 素材 | CER macro 736 → 0.7MP | exact 736 → 0.7MP | timing precision 736 → 0.7MP | split 736 → 0.7MP |
|---|---:|---:|---:|---:|
| Zootopia 1080p | 17.27% → **3.67%** | 72.6% → **92.0%** | 90.1% → 90.4% | 1 → 4 |
| Zootopia 720p | 13.18% → **4.83%** | 76.6% → **92.3%** | 96.2% → 95.1% | 0 → 3 |
| Zootopia 480p | 8.78% → **3.13%** | 84.4% → **94.7%** | 96.2% → 95.0% | 1 → 0 |
| mixed_multiline | 55.12% → **7.50%** | 20.0% → **90.0%** | 100% → 100% | 0 → 0 |
| latin_motion | 37.62% → **0.00%** | 11.1% → **100%** | 22.0% → **39.1%** | 8 → 2 |

- 5 个素材 CER/exact 全部改善，方向不是旧脚本匹配口径造成的假象。
- 条目级并非全面改善：480p timing recall 87.4% 略低于 88.5%，720p/480p precision 各降
  约 1pp，1080p/720p split 增加；latin 的 39.1% precision 仍然很低。
- 旧脚本 `analyze_quality.py` / `analyze_matrix.py` 允许多个 GT 复用同一条 det，且不惩罚
  未匹配 det，命中数 86/87 与官方 recall 不可直接比较。

产物在 `debug/ab-det-budget/`（`debug/` 不入库）。

## 4. 未做与风险

1. **未切换默认值**：产品仍走 736。切换会改变 det 框与 SRT 输出，必须作为独立决策，
   重新冻结 RapidOCR parity / golden hash，不能以性能优化名义静默合入。
2. **未覆盖全部路径**：本次 A/B 只测 1080p bottom-crop。实验文档已提示合入前需补真实
   低分辨率源（非下采样合成）、竖屏/超宽条带、全帧路径与 region 检测路径，并复跑
   `paddle_quality` 全门。本报告的官方诊断只对已有 SRT 重新评分，不是全门运行。
3. **替代解释未完全排除**：实验文档要求用 Python/RapidOCR 参考实现在同一 crop、同一 736
   下复现“框破碎”，确认差异来自几何而非 C++ DB 后处理移植偏差。
4. **策略配置面已收敛**（Phase 18 D01）：产品路径改用 `--det-pixel-budget` / Server
   `det_pixel_budget`；env 只对 adapter 3 参入口有效。默认仍 736；0.7MP
   是宽平台上的可解释折中，不是精确最优点（实验 4.2：质量在 0.4–0.8MP 为平台）。
5. **旧质量口径有偏**：实验期 matched CER 允许多个 GT 复用同一条 det，且不惩罚未匹配 det；
   官方 one-to-one 复核方向不变，但绝对数和命中数都不同。合并判断必须以 3.4 的官方指标为准。
6. **latin 工件曾混淆**：工作树里 `q_latin_motion_p*.srt` 被后续 `--script cjk` 运行覆盖，
   直接用会得到两边 CER 100%。有效证据是 2026-10-21 用 `--script latin` 的重跑（3.4）。
7. **计时样本有限**：各配置以单次为主；但无计时与计时开的两组独立 A/B 方向一致，且输出
   哈希分别固定为 `56b1…` / `e667…`，2.9–3.1× 的差距远超运行噪声。
8. **预算值有调参集复用**：0.7MP 在 480p 扫描中选定，Zootopia 三档同源；两个独立 fixture
   也改善，但 0.7MP 不是普适最优点，需在未参与调参的素材上复核 0.4–0.8MP 平台。

## 5. 下一步建议

1. **决策项**：以 ADR 形式决定默认策略（0.7MP 或 `clamp(native, 0.6MP, 1.0MP)`），
   同步重冻 parity/golden，并把预算值纳入配置而非 env。
2. **质量门**：按实验文档第 6 节补全路径与素材后，用 `paddle_quality` 全门复跑，并把
   split/FP 与 timing precision 纳入验收，再评估是否可以直接替换 736。
3. **下一步性能方向**：0.7MP 后 det 仍占约 23s / 41.5s。若同一段内布局稳定，可探索
   **det-once + rec-many**：段首检测一次，后续共识帧复用框只跑 Rec，并用掩码/框几何
   做布局守卫；这比“每帧重新 det”更接近 Proposal 里“一行一次 Rec”的目标。

## 6. 复现

```bash
# 构建（当前工作树）
cmake -S cpp -B build/cpp-rel -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DSUBLIFT_REQUIRE_OPENCV=ON -DSUBLIFT_ENABLE_VISION=ON -DSUBLIFT_ENABLE_PADDLE=ON
cmake --build build/cpp-rel -j 8

# 默认 736
SUBLIFT_WORKER_PATH=build/cpp-rel/bin/sublift_worker \
  build/cpp-rel/bin/sublift extract debug/Zootopia_clip_1080p.mp4 \
  -o debug/ab-det-budget/baseline.srt --engine paddle --fps 5 --script cjk

# 0.7MP 预算
SUBLIFT_WORKER_PATH=build/cpp-rel/bin/sublift_worker \
  build/cpp-rel/bin/sublift extract debug/Zootopia_clip_1080p.mp4 \
  -o debug/ab-det-budget/budget-0.7mp.srt --engine paddle --fps 5 --script cjk \
  --det-pixel-budget 700000

# latin fixture 必须用 latin；否则输出是 cjk 垃圾，不能用于质量评估
SUBLIFT_WORKER_PATH=build/cpp-rel/bin/sublift_worker \
  build/cpp-rel/bin/sublift extract debug/paddle-quality/latin_motion_180s.mp4 \
  -o debug/ab-det-budget/latin-736.srt --engine paddle --fps 5 --script latin
SUBLIFT_WORKER_PATH=build/cpp-rel/bin/sublift_worker \
  build/cpp-rel/bin/sublift extract debug/paddle-quality/latin_motion_180s.mp4 \
  -o debug/ab-det-budget/latin-budget.srt --engine paddle --fps 5 --script latin \
  --det-pixel-budget 700000
```

阶段归因在命令前加 `SUBLIFT_STAGE_TIMING=1`；官方质量诊断见
[`det-input-geometry.md`](det-input-geometry.md) 第 7 节。
