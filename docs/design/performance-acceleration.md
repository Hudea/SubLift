# SubLift 性能加速方案

> **状态**：分析文档，2026-10-19；P0（S0 + O1–O8）已于 2026-10-20 实施并实测，端到端收益 −0.7%（噪声内），P0 已关闭（见第 7 节）；Phase 16 已以负向结论 `done` 收口（D04），P1/P2 未实施。各节优先级判断以第 7 节为准。
> **分析对象**：Phase 13 之后的 Native-only 产品运行时（C++ Core / Pipeline / Application / Adapters / Worker / Native Server）。
> **数据基础**：版本化归因基线 `benchmark/baselines/performance-attribution-baseline.md`、`docs/design/ocr-performance-attribution.md` 的决策表，以及对 `cpp/` 全树的代码级普查（带 `file:line` 证据）。
> **文档定位**：优化候选清单、瓶颈归因与优先级排序。实施任一措施前，须按项目契约登记维护 Phase（2–8 个纵向 Deliverable）并通过相应验证门；本文不自行批准代码变更。

---

## 1. 结论摘要

当前产品的性能结构由三个事实决定：

1. **单消费者串行是吞吐上限**。Pipeline 与 OCR 实例按契约串行消费（`cpp/include/sublift/pipeline/pipeline.hpp:44-45`、`docs/cpp/native-architecture.md:56`），端到端速度 ≈ 最慢单消费者阶段的速度。归因基线中 OCR 占 core wall **51.65%**，是唯一的结构性上限。
2. **ROI 数据通路已完成第一道降本**。raw RGB 输出减少 91.94%、`frame_materialize` 降为 2.5%，大图像搬运不再是主要矛盾（ADR 证据：Full/ROI 结果 hash 均为 `b2d35c1e25f156e1`）。
3. **并发恒为 1 且模型按 job 重载**。Native Server 硬编码 `JobManager(1, 50)`（`cpp/src/server/http_server.cpp:118`），Paddle 引擎（3 个 ORT 会话 + `Ort::Env`）每 job 创建销毁（`cpp/src/application/bridge.cpp:400,624` → `cpp/src/adapters/paddle/paddle_ocr.cpp:174-242`），批量场景下模型加载无法摊销。

据此，本方案的优先级判断是：

- **P0（先做，低风险）**：一批"数值完全等价"的纯工程优化—— patrol 掩码缓存、SSIM 缓冲复用、热路径去零填充、OCR 前处理去冗余拷贝、SSE 事件查询索引化、进度节流、二进制路径缓存。不触碰任何数值通路，不需要新基线。
- **P1（基准门后做）**：引擎跨 job 缓存与 warmup、job 间并发可配置化、持久化写路径、智能选区 ROI 化、取消异步化。每一项都需要 ADR 级决策与基准/质量门，因为涉及资源生命周期、并发契约或既有冻结决策（ADR-0025/0028）。
- **P2（评估或长期）**：RGB→BGR 与灰度融合（需逐位 parity 验证）、ffmpeg 硬件解码评估、跨段 OCR 记忆化、GPU 执行后端（Phase 15，最大长期杠杆，硬件阻塞中）。

**一个前置结论**：现有归因基线来自 Python/Vision 时代；C++ Native + Paddle 路径的阶段分布（特别是 det/cls/rec 三段推理与标量前处理的占比）**尚未测量**。第一批实施的第一个动作必须是建立 C++ Native 归因基线（benchmark harness 已就绪），否则后续收益数字没有对照。

---

## 2. 瓶颈分析

按四个域列出带证据的瓶颈。编号 `B*` 在后续优化措施中引用。

### 2.1 每帧签名与变化检测（基线占比：signature 15.98% + changepoint 11.95% ≈ 28%）

| # | 瓶颈 | 证据 | 说明 |
|---|---|---|---|
| B1 | RGB→BGR 全帧标量交换 | `cpp/src/pipeline/pipeline.cpp:516-543` | 每帧分配 W·H·3 缓冲（1920×324 时 1.9MB）并逐字节标量交换，为复现 Python 颜色 quirk；随后 `compute_signature` 再灰度化一次（`cpp/src/pipeline/signature.cpp:33-37`）。基线 `color_convert` 占 2.19%。 |
| B2 | 全分辨率自适应阈值 + 形态学 | `cpp/src/pipeline/signature.cpp:53-64` | 每帧 3 个全尺寸操作（`adaptiveThreshold` GAUSSIAN_C、`MORPH_OPEN`、`countNonZero`），处理对象是完整 ROI crop，无降采样；`getStructuringElement` 每帧重建。 |
| B3 | SSIM patrol 每 3 帧引爆一次全尺寸 CV_64F 计算 | `cpp/src/pipeline/changepoint.cpp:191-207` → `cpp/src/pipeline/signature.cpp:86-134,171-185` | 稳态下约 1.67 次/秒（5fps）；单次 patrol ≈ 20+ 个全尺寸 float64 分配与遍历（2 次掩码前处理 + 5 次 GaussianBlur + 表达式临时），1920×87 时约 27MB 内存流量，1920×324 时约 100MB。这是 changepoint 占比的主要来源。 |
| B4 | 锚帧掩码每次 patrol 重算 | `cpp/src/pipeline/signature.cpp:179-184` | 锚 crop 在 In/CHANGE 之间不可变（`changepoint.cpp:76`），但 `compute_foreground_ratio_mat` 每次 patrol 对锚帧完整重算二值化。 |
| B5 | 每稳定帧 2 次整帧深拷贝 | `cpp/src/pipeline/pipeline.cpp:383-425` | `segment_stable_frame_` 与 `samples.back()` 在段内每个无事件稳定帧各自整体替换（各 1.9MB@324）；段关闭时实际只用到 4 帧样本。 |
| B6 | `ImageBuffer` 零填充后立即覆盖 | `cpp/src/core/image.cpp:83` | `storage_.assign(nbytes, 0)`；抽帧缓冲（`ffmpeg_extractor.cpp:275`）与 BGR 缓冲构造后都被整块覆盖，1080p 全帧每帧白扔 6.2MB memset。 |
| B7 | patrol 使用默认 `SignatureConfig{}` 而非 `config_.signature` | `cpp/src/pipeline/changepoint.cpp:204-206` | 既是正确性/一致性疑点，也意味着无法通过调参降低 patrol 成本。 |

### 2.2 OCR 适配器（基线占比：ocr 51.65%，Vision 路径；Paddle 路径未测）

| # | 瓶颈 | 证据 | 说明 |
|---|---|---|---|
| B8 | 每次 recognize 两次全帧 memcpy | `cpp/src/adapters/paddle/ppocr_det_preprocess.cpp:98,142-144` | `working = source.clone()` 从未被原地修改，随后又整帧拷入 `std::vector`；纯冗余搬运。 |
| B9 | 每 job 重建 3 个 ORT 会话 + `Ort::Env`，无 warmup | `cpp/src/adapters/paddle/paddle_ocr.cpp:174-242`；调用点 `cpp/src/application/bridge.cpp:400,624` | 模型加载（mmap + 图优化 + 字典 SHA 校验）每 job 重复；`Ort::Env` 按引擎构造，反复创建/销毁 ORT 全局线程池；全库无 warmup，首帧推理含冷启动。 |
| B10 | batch 输入双重清零 + 每批分配 | `cpp/src/adapters/paddle/paddle_ocr.cpp:308-311,405`、`cpp/src/adapters/paddle/ppocr_crop_cls.cpp:226-231` | `std::vector<float>` 值初始化清零后 `prepare_padded_tensor` 内再 memset，且每批次重新分配（cls 批默认 6×3×48×192×4B ≈ 663KB）。 |
| B11 | ORT 线程/arena 设置为 RapidOCR parity 冻结值 | `cpp/src/adapters/paddle/paddle_ocr.cpp:189-203`、`cpp/include/sublift/adapters/paddle.hpp:27` | intra_op 钉在 4（与核心数无关）、`DisableCpuMemArena()`、inter_op 未设置。ADR-0025/0028 已把 canonical dylib SHA + 4 线程 + Rec batch=6 冻结，并设 `source_output_sha256_exact` 硬门。 |
| B12 | server 智能选区对全帧跑完整 OCR | `cpp/src/server/region_detector.cpp:89-125` | 最多 7 个时间点，每点对 ≤1920px 全帧执行 det+cls+rec；字幕只可能在底部带，属于可 ROI 化的明显浪费。 |
| B13 | 手写标量前处理（双线性 resize、NCHW 归一化） | `cpp/src/adapters/paddle/ppocr_crop_cls.cpp:93-194,242-253`、`ppocr_det_preprocess.cpp:187-200` | 字节级 parity 约束（复刻 RapidOCR），是除 ORT 外每 crop 的主要 CPU 成本；不可轻易向量化。 |

### 2.3 媒体通路

| # | 瓶颈 | 证据 | 说明 |
|---|---|---|---|
| B14 | 每帧新建 `ImageBuffer`（含零填充） | `cpp/src/adapters/ffmpeg/ffmpeg_extractor.cpp:275` | 见 B6；move 语义本身是干净的（管道→消费者一次拷贝）。 |
| B15 | ffmpeg/ffprobe 二进制解析无缓存 | `cpp/src/adapters/ffmpeg/resolve_bin.cpp:19-39`、`cpp/src/models/resource_locator.cpp:72-84` | 每次 `extract()` 与单帧 API 都重建 `ResourceLocator` 扫 PATH。 |
| B16 | 每 job 多次子进程 spawn 且无调优参数 | `ffmpeg_extractor.cpp:249`（抽帧）、`plan_frame_io.cpp:106` + `bridge.cpp:648`（1–2 次 ffprobe） | 无 `-threads`/`-hwaccel`；短视频场景进程启动开销占比可观。 |
| B17 | 单帧 API 双缓冲 | `process_utils.cpp:156` → `ffmpeg_extractor.cpp:431-444` | stdout 先累积进 `std::string` 再 memcpy 进 `ImageBuffer`。 |
| B18 | 无硬件解码 | `ffmpeg_extractor.cpp:194-210` | 纯 CPU 解码；macOS 有 videotoolbox 空间，但涉及 parity 与平台差异，列为评估项。 |

### 2.4 宿主调度与 I/O（Server / Worker）

| # | 瓶颈 | 证据 | 说明 |
|---|---|---|---|
| B19 | 并发恒为 1，第二个 job 排队 | `cpp/src/server/http_server.cpp:118`、`cpp/src/server/job_manager.cpp:56-59,104` | `max_concurrent_jobs` 硬编码 1；BridgeHandler 另有互斥双保险（`bridge.cpp:304-310`）。批量吞吐上限 = 单线程 OCR 吞吐。 |
| B20 | SSE 事件查询 O(N²) + 无界 history + 三次 JSON 深拷 | `cpp/include/sublift/server/job_manager.hpp:187-240` | `fetch_after` 每次从头部线性全扫；`to_sse_string` 再拷一次；`history_` deque 无上限。 |
| B21 | 全量状态重写持全局锁跨 fsync | `cpp/src/server/job_manager.cpp:226-275` + 调用点持 `mutex_` | 每次保存遍历全部 job 的全部 entries 序列化 + `root.dump(2)` + fsync；每 job 生命周期 3–4 次。 |
| B22 | 取消路径阻塞 join | `cpp/src/application/bridge.cpp:258-260`、`cpp/src/server/job_manager.cpp:71-78,157-191` | HTTP cancel 在持锁条件下同步等待 ffmpeg 死亡 + 线程回收；shutdown 路径同样持锁 join。 |
| B23 | `thread_pool_size` 是死配置 | `cpp/include/sublift/server/http_server.hpp:20`（无任何读取） | httplib 用默认池；阻塞型端点（frame spawn、remux、detect-region、指纹扫描）与长命 SSE 争用同一池。 |
| B24 | frame 模式每帧一条 ProgressMsg | `cpp/src/application/bridge.cpp:462-467` | UDS 上每帧一条 JSON 的事件风暴（仅 macOS 路径；path 模式已按 5 帧节流，`bridge.cpp:664-674`）。 |
| B25 | 每请求全量系统探测 | `cpp/src/server/routes.cpp:1259` → `paddle_ocr.cpp:117-150` | POST /api/jobs 每次重新 probe 模型 bundle + 读 manifest；结果实质静态，未缓存。 |

---

## 3. 优化措施

按算法复杂度、资源调度、并发处理、缓存机制四个维度组织。每项标注：**优先级**（P0 立即 / P1 基准门后 / P2 评估或长期）、**预期收益**、**风险与缓解**、**验证方式**。

收益估算的参照系：Zootopia 1920×1080 / 5fps / ROI 1920×87 / 1271 帧的 core wall 9,341.7ms（Vision 基线）。Paddle 路径与 1080p bottom-crop（1920×324）场景的绝对值需以第 3.5 节的新基线为准。

### 3.1 算法复杂度与数据搬运

#### O1. SSIM patrol 锚帧掩码缓存 —— P0

- **措施**：`ChangePointDetector` 在 In/CHANGE 更新 `anchor_crop_` 时，一并缓存其 `compute_foreground_ratio_mat` 产出的 binary mask；`maybe_patrol` 只对当前帧计算掩码，与缓存掩码做 SSIM。
- **证据**：B4（`signature.cpp:179-184`、`changepoint.cpp:76`）。
- **预期收益**：patrol 前处理成本减半（两份掩码计算变一份），changepoint 阶段（基线 11.95%）预计下降 20–30%；不改变任何数值——相同输入、相同算法、相同参数，只是消除重算。
- **风险**：极低。需保证缓存掩码与锚 crop 严格同生命周期（In/CHANGE 同步替换）；注意 B7（patrol 用 `SignatureConfig{}`）——缓存键必须包含实际生效的参数，后续若修 B7 不产生脏缓存。
- **验证**：现有 `[changepoint]`/`[signature]` CTest + golden hash `b2d35c1e25f156e1` 不变。

#### O2. `compute_ssim_mat` 预分配缓冲复用 —— P0

- **措施**：将 patrol 路径的 SSIM 计算改为调用方持有的可复用 `Mat` 工作集（`convertTo`、`GaussianBlur`、表达式临时全部求值到预分配目标），消除每次 patrol 约 20 次 MB 级 malloc/free；运算种类与顺序保持不变。
- **证据**：B3（`signature.cpp:86-134`，t1–t4 MatExpr 临时）。
- **预期收益**：与 O1 叠加后 patrol 单次成本预计下降 30–50%（内存流量与分配器churn 消除，计算量不变）；1920×324 场景每次 patrol 减少约 100MB 瞬时内存流量。
- **风险**：低。数值等价的条件是运算顺序与数据类型不变（保持 CV_64F 与相同 blur 参数）；禁止顺手改 CV_32F（那会改变 parity）。
- **验证**：同 O1；另加一条"patrol 开启/关闭输出逐位一致"的回归。

#### O3. 热路径 `ImageBuffer` 去零填充 —— P0

- **措施**：为"构造后立即整块写入"的内部场景提供免初始化构造/工厂（如 `ImageBuffer::allocate_uninitialized(w,h,fmt)` 或显式 `skip_zero_fill` 标签），仅用于抽帧循环缓冲（`ffmpeg_extractor.cpp:275`）与 pipeline BGR 缓冲（`pipeline.cpp:531`）。公共 API 的默认构造保持零填充。
- **证据**：B6（`image.cpp:83`）。
- **预期收益**：每帧消除一次全尺寸 memset（1080p 全帧 6.2MB、ROI 324 条带 1.9MB）；`extract_wait` + `frame_materialize` + BGR 分配相关成本下降，预计 core wall 1–3%。
- **风险**：中低——未初始化内存不得泄漏到任何输出或错误路径。缓解：工厂不公开到 Ports 之外；调用点保证"分配后、任何读取前"整块写入；ASan/UBSan 构建下加一条读写探针测试。
- **验证**：现有 CTest 全量 + golden hash 不变；新增 UBSan 构建跑抽帧契约测试。

#### O4. `prepare_global_image` 去冗余 clone 与二次拷贝 —— P0

- **措施**：`ppocr_det_preprocess.cpp:98` 的 `source.clone()` 删除（该副本从未被原地修改）；`result.rgb` 的整帧 `assign` 改为对已就地处理结果的引用/移动，使每次 recognize 的全帧 memcpy 从 2 次降为 0–1 次。
- **证据**：B8。
- **预期收益**：Paddle 路径每次 OCR 调用省 2 次全帧拷贝；在 1920×324 输入上约 1.9MB×2/次。OCR 是 core wall 最大项（51.65% Vision 基线；Paddle 路径预计更高），前处理占比预计 10–20%，此项可拿下一半以上。
- **风险**：低。字节级输出不变（纯搬运消除）；需确认无任何依赖 clone 副作用的代码路径（该函数内所有操作均为重新赋值）。
- **验证**：Paddle parity 门（`scripts/parity/check_paddle_det_parity.py` 等）+ 三来源 SRT SHA 不变。

#### O5. SSE 事件存储索引化 + history 有界 —— P0

- **措施**：`JobEventStream` 的 `history_` 增加 seq→索引映射（或环形缓冲 + 起始 seq），`fetch_after(cursor)` 从 O(N) 全扫改为 O(k) 切片；history 设上限（如每 job 最近 2000 条 + 终态快照常驻），`to_sse_string` 避免额外 JSON 深拷（move 或直接 dump）。
- **证据**：B20（`job_manager.hpp:187-240`）。
- **预期收益**：长任务（数千条字幕）SSE 轮询/重连从 O(N²) 降为 O(k)；每条事件 JSON 拷贝从 3 次降为 1 次；history 内存有界。
- **风险**：低。语义约束：Last-Event-ID/cursor 续传契约（HURDLES 12512）必须保持——裁剪只能丢弃"已被确认消费且超过上限"的旧事件，且需保留足够回放窗口或让客户端回退到终态快照。
- **验证**：Web 测试（`npm --prefix apps/web test`）+ server 契约测试中增加长事件序列的 cursor 续传用例。

#### O6. 进度消息节流（frame 模式）—— P0

- **措施**：frame 模式 ProgressMsg 从"每帧一条"改为按时间窗节流（如 ≥200ms 或 ≥N 帧一条，与 path 模式对齐）；pct 语义与去重规则不变。
- **证据**：B24（`bridge.cpp:462-467`）。
- **预期收益**：消除 UDS 每帧 JSON 序列化 + 2 次 write 系统调用；macOS GUI 在 5fps 下每 200ms 一条已足够平滑。
- **风险**：低。纯频率变化；需确认 Swift 客户端进度条在节流后仍平滑（200ms 粒度对 5fps 采样无损）。
- **验证**：Swift 测试 + 手动 GUI 冒烟。

#### O7. 二进制路径解析缓存 + 探测合并 —— P0

- **措施**：`resolve_ffmpeg_bin`/`resolve_ffprobe_bin` 结果按进程缓存（首次解析后复用，路径变更由显式失效处理）；`plan_frame_io` 的探测与 bridge 的时长探测合并为一次 ffprobe 调用。
- **证据**：B15、B16。
- **预期收益**：每 job 省 1 次子进程 spawn 与重复 PATH 扫描；短视频场景启动开销下降。
- **风险**：低。缓存需处理 PATH 环境变化（进程启动后基本不变；文档化为进程级缓存即可）。
- **验证**：ffmpeg 适配器契约测试 + 端到端 golden。

#### O8. 系统信息探测结果缓存 —— P0

- **措施**：`collect_system_info` / `probe_capabilities` 结果按"manifest 指纹 + 文件 mtime"缓存，带 TTL；运行时失败立即使缓存失效（与 GPU 设计文档第 5 节"探测不在每次 GET system/info 时重复加载模型"一致）。
- **证据**：B25。
- **预期收益**：POST /api/jobs 与 GET system/info 从"每次读 manifest + 扫 bundle"降为缓存命中；批量创建 N 个任务时省 N−1 次探测。
- **风险**：低。能力是部署级静态事实；失效条件要覆盖模型文件替换（Phase 14 更新镜像的场景由重启解决）。
- **验证**：server 契约测试（能力变更后缓存失效用例）。

#### O9. 智能选区底部 ROI 化 —— P1

- **措施**：`RegionDetector` 的候选帧识别从"全帧完整 OCR"改为"先按底部候选带裁剪（或降采样）再 OCR"，提前停止条件与评分阈值保持不变；选区结果几何仍映射回 source-frame 坐标。
- **证据**：B12（`region_detector.cpp:89-125`）。
- **预期收益**：选区路径（Web 单视频"智能选区"按钮）单次耗时预计下降 50%+（识别面积从全帧降为条带）；不影响提取主路径。
- **风险**：中——选区是质量相关功能，裁剪/降采样可能改变检出框。缓解：保留全帧模式作为 fallback；用现有 GT 验证选区框 IoU 不降。
- **验证**：region detector 契约测试 + 真实素材选区对比（框坐标集合）。

#### O10. stable/sample 帧克隆策略重构 —— P1

- **措施**：将"每稳定帧 2 次全帧 clone"改为：段内维护一个小型（如 8 槽）共享 owned-frame ring，稳定帧只写入/替换 ring 槽（1 次拷贝），样本帧记录"ring 槽 + 时间戳"引用；段关闭时按需物化。或退而求其次：clone 时只存 ROI crop + 共享存储 view（需确认 OCR 输入几何当前就是对全帧做 crop——若是，可直接存 crop）。
- **证据**：B5（`pipeline.cpp:383-425,545-547`）。
- **预期收益**：稳态每帧从 2 次全帧拷贝降为 1 次（或接近 0，若 OCR 只需 crop）；1920×324 场景每稳态帧省 1.9MB 拷贝。
- **风险**：中——`SegmentEvent` 的 owned Frame 所有权契约（`pipeline.hpp:20-28`）与 OCR 锚帧选取逻辑依赖全帧；重构必须保持"段关闭时可独立取到任一候选帧"的语义。缓解：先做"只 clone crop"的最小版本，ring 方案作为第二步。
- **验证**：`[pipeline]` CTest + golden hash 不变 + 内存峰值对比（长视频 RSS 不涨）。

#### O11. RGB→BGR 与灰度化融合 —— P2

- **措施**：将"逐像素 BGR 交换 + cvtColor RGB2GRAY"融合为单遍"按互换权重直接灰度化"，省掉每帧 1.9MB 分配与两次全帧遍历。
- **证据**：B1（`pipeline.cpp:516-543`、`signature.cpp:33-37`）。
- **预期收益**：消掉 `color_convert`（基线 2.19%）并减少 signature 的输入遍历；合计约 core wall 2–4%。
- **风险**：高（parity）——净数学效果等价，但定点舍入路径不同，dHash/阈值结果可能逐位变化，golden hash `b2d35c1e25f156e1` 可能不保。缓解：实现后逐位比对 Full/ROI 双路径 hash；不相等则需重新冻结 golden（属于基线变更决策，不是优化附带品）。
- **验证**：parity harness 双路径 hash + 三来源 SRT SHA。

#### O12. dHash / SSIM 降采样计算 —— 不做（记录）

- dHash 先缩到 9×8 再阈值化、SSIM 在半分辨率上计算，都会改变 hash 与 patrol 判定（parity 锁定）。在"重建 golden 基线"成为独立决策之前，本条记录为**不可行**，防止后续重复提议。

#### O13. 跨段 OCR 记忆化 —— P2（观察）

- **措施**：对段间重复出现的相同画面（crop 内容寻址）跳过重复 OCR。
- **预期收益**：不确定——段间画面重复率取决于内容（循环片头/静止画面可能受益，对白字幕基本无重复）。
- **风险**：缓存键（内容 hash）本身有计算成本；误命中会静默改变输出。**建议先测量重复率再决定**，不作为承诺项。

### 3.2 资源调度

#### O14. OCR 引擎跨 job 缓存 + 单 `Ort::Env` + warmup —— P1

- **措施**：Server/Worker 进程内按 (engine, provider, model bundle 身份) 缓存 OCR 引擎实例；job 获取"租约"（同时同引擎只服务一个 job，保持串行契约），job 结束归还而非销毁；`Ort::Env` 进程级单例；引擎创建后执行一次小 fixture warmup 推理。
- **证据**：B9（`bridge.cpp:400,624`、`paddle_ocr.cpp:174-242`）。
- **预期收益**：批量场景每 job 省一次模型加载（3 会话构建 + 字典 SHA + 图优化，秒级）；冷启动首帧成本消除。这是批量吞吐的第二大杠杆（仅次于并发）。
- **风险**：中高。
  - 常驻内存：3 个 ORT 会话 + 权重常驻（HURDLES 记录 Paddle 路径 RSS ~1.7GB/ job，缓存后变为常驻基线）；需要内存准入与显式驱逐策略。
  - 能力失效：模型文件/manifest 变更后缓存引擎必须失效（按 bundle 身份 + mtime）。
  - 与 Phase 15 设计的张力：`docs/design/gpu-execution.md` 第 6 节明确"第一版沿用每任务持有 OCR 会话，暂不同时引入跨任务会话缓存"——GPU 路径引入缓存前需先过该设计的 gate/lease 模型。**CPU 路径实施本项需单独 ADR**，并与 GPU 路线协调（缓存的租约模型应同时满足 CPU 与未来的 `OcrExecutionGate`）。
- **验证**：benchmark 对比"冷/热"两种 job 序列的 wall 与 RSS；能力变更失效用例；长跑内存平稳（无累积）。

#### O15. job 间并发可配置化 + 准入控制 —— P1

- **措施**：`max_concurrent_jobs` 从硬编码 1 改为部署级配置（默认仍为 1，保持现有语义）；每并发 job 独立 Pipeline + 独立引擎租约；按可用内存与引擎常驻成本设置准入上限（估算：每 Paddle job ≈ 会话权重 + 每帧缓冲 + patrol 工作集）。
- **证据**：B19（`http_server.cpp:118`）。
- **预期收益**：Web/Server 批量吞吐随并发近线性提升（受 ORT 线程争用与内存带宽制约，需实测标定）；macOS Task Center 本身串行（Swift 侧队列），不受影响。
- **风险**：中高。
  - Pipeline/OCR 单 consumer 契约不受影响（每 job 独立实例），但 ORT intra_op=4 × N 并发会造成 CPU 超订——需要与 O16 的线程配置联动评估。
  - 持久化写放大（N 个 job 的事件/保存争用同一 `mutex_`）在 O17 之后再做。
  - GPU 场景由 `OcrExecutionGate` 独占 lease 管理，不得与 CPU 并发模型混谈。
- **验证**：server 契约测试（并发上限、排队、取消竞态——HURDLES 12502 的教训）+ 批量 benchmark（N=2/4 的 wall 与 RSS）。

#### O16. ORT 线程/arena 参数重新评估 —— P1（ADR 门）

- **措施**：在 canonical ORT dylib（ADR-0025 固定 SHA）不变的前提下，重新测量 intra_op=4 / default / `hardware_concurrency` 与 arena 开/合的聚合耗时与内存，作为是否调整默认值的依据。
- **证据**：B11（`paddle_ocr.cpp:189-203`）。
- **预期收益**：不确定。ADR-0025 的 sweep（1/2/4/default = 1448/857/616/650ms aggregate median）显示 4 已是当时最优，但那是单 job 串行场景；多核服务器与并发场景（O15）下的最优点可能不同。
- **风险**：高——线程数与 arena 都可能改变推理尾数（ADR-0028 的 Rec batch 教训：微基准收益不等于产品语义不变）。任何默认值变更必须过 `source_output_sha256_exact` 硬门与三来源质量门，并以 ADR 记录。
- **验证**：`scripts/parity/check_paddle_perf.py` + `check_paddle_gate.py` 全门。

#### O17. 持久化写路径优化 —— P1

- **措施**：（a）保存从"全量重写"改为"脏标记 + 增量/合并写"；（b）fsync 移出全局 `mutex_`（锁内完成内存快照与临时文件写，锁外 fsync + rename，用独立 I/O 锁保证写顺序）；（c）`root.dump(2)` 美化输出改为紧凑输出（文件仅用于恢复，不需要人读）。
- **证据**：B21（`job_manager.cpp:226-275`）。
- **预期收益**：大 batch（数百 job × 数千 entries）的全局 API 停顿从"每次保存 O(总 entries) + fsync"降为 O(增量)；`get_job`/`create_job`/`cancel_job` 不再被 fsync 阻塞。
- **风险**：中——崩溃一致性与"损坏/未知版本 fail-closed"契约必须保持；锁外 fsync 需要重新论证断电/崩溃场景的状态文件有效性。缓解：保持临时文件 + 原子 rename 不变，只改"何时持锁"；增量格式保持版本化（v1→v2 迁移按 GPU 设计文档的 v2 先例处理）。
- **验证**：server 契约测试增加"保存中崩溃恢复"用例（kill -9 后 load_state 行为不变）+ 大 batch 保存延迟基准。

#### O18. 取消路径异步化 —— P1

- **措施**：HTTP/worker cancel 置标志 + 杀 ffmpeg 子进程后立即返回；线程 join 交还 worker 线程自身（或专责回收线程）执行，设合理超时；移除 shutdown 路径持锁 join。
- **证据**：B22（`bridge.cpp:258-260`、`job_manager.cpp:71-78`）。
- **预期收益**：取消请求延迟从"ffmpeg 死亡 + 线程回收"降为"标志 + 信号"（HURDLES feat-030 记录取消响应 0.108s 的目标量级）；shutdown 不再被长 join 阻塞。
- **风险**：中——Worker IPC 契约的取消验收目标（1s 停止有效工作、5s 允许重启）必须保持；双 cancel 竞态是 12502 修过的 bug，重构需保留其防护（move-join 防护）。
- **验证**：worker IPC 契约测试（取消时延、双 cancel、取消后立即可启动新 job）。

#### O19. 线程池配置生效 + 阻塞端点隔离 —— P1

- **措施**：`thread_pool_size` 真正接入 httplib（或删除该死配置）；将 `/api/video/frame`、`/api/video/stream`（remux）、`/api/video/detect-region` 等阻塞型端点移到独立池，避免与长命 SSE 争用。
- **证据**：B23。
- **预期收益**：多标签页 + 批量预览场景下 API 响应不再被 SSE 连接数挤压；服务器参数可运维。
- **风险**：低—中。主要是 httplib 线程模型的适配成本；行为变化需契约测试覆盖。
- **验证**：server 契约测试（并发 SSE + 阻塞端点混合负载）。

#### O20. ffmpeg 解码参数调优（`-threads`、hwaccel 评估）—— P2

- **措施**：抽帧命令增加显式 `-threads`（按可控默认）；videotoolbox 硬件解码作为**评估项**单独基准。
- **证据**：B16、B18。
- **预期收益**：`extract_wait`（基线 15.03%）可能下降；hwaccel 在 H.264 1080p 上通常显著，但 ROI 通路已把输出量降为 8.06%，解码本身成为主要项，收益需实测。
- **风险**：hwaccel 涉及像素路径与时间戳 parity（滤镜链在硬解输出上的行为差异）、平台差异（仅 macOS）与失败回退语义（产品契约禁止静默降级——硬解失败必须明确报错或明确回退策略，不能暗中切）。`-threads` 低风险。
- **验证**：A/B 基准（同机同媒体 warmup+measured median）+ golden hash 不变（`-threads`）；hwaccel 需先出 parity 证据再谈默认。

### 3.3 并发处理

**总原则：不做流水线并发，只做"资源级"并发。**

- Phase 4.1 的 producer/consumer 实验已归档为负向证据（ROI 后 extractor 足够快、Vision 主导单消费者、队列调度开销吞没收益，`docs/HURDLES.md:218-231`）。在单消费者 OCR 的时间占比下降一个数量级之前，**重新引入 extractor↔OCR 流水线没有依据**；若未来 OCR 通过 O4/O14/O16 显著变快，需以新的归因数据重新评估，而不是复用旧结论。
- 可行的并发维度只有三个：
  1. **job 间并发**（O15）——每 job 独立 Pipeline/引擎租约，不违反单 consumer 契约；
  2. **ORT 内部并行**（O16）——已冻结，重评需 ADR 门；
  3. **引擎内部已有的批处理**——cls/rec 批默认 6（ADR-0028 冻结），det 单图；批大小变更同样受 `source_output_sha256_exact` 门约束。
- GPU 路径的并发由 `OcrExecutionGate`（Phase 15 设计第 6 节）统一管理，与本方案的 CPU 并发模型是同一问题的两个部署面，实施 O15 时应按该设计的 lease 语义预留接口。

### 3.4 缓存机制

| 缓存 | 措施 | 优先级 | 说明 |
|---|---|---|---|
| 段内锚帧掩码 | O1 | P0 | 生命周期 = 段内 In/CHANGE 区间；数值等价。 |
| OCR 引擎/会话 | O14 | P1 | 跨 job；需租约、失效与内存准入；与 GPU gate 设计协调。 |
| 模型 bundle 校验 | 随 O14 | P1 | manifest SHA 校验结果按 (path, size, mtime) 缓存，避免每 job 重算；模型替换即失效。 |
| 系统能力探测 | O8 | P0 | TTL + 失效。 |
| SSE 事件 history | O5 | P0 | 有界 + cursor 续传。 |
| 视频 remux 缓存 | 已存在 | — | `ffmpeg_extractor.cpp:481-483` 的磁盘缓存保持现状，记录备查。 |
| 跨段 OCR 结果 | O13 | P2 | 先测重复率再决定。 |

### 3.5 前置动作：建立 C++ Native 归因基线

现有阶段归因（ocr 51.65% 等）来自 Python/Vision 路径。C++ Native + Paddle 的分布未知，且直接决定 O4/O9/O16 的收益估算是否成立。

- **措施**：用现有 benchmark harness（`benchmark/configs/` + `scripts/parity/check_paddle_perf.py`）在同一固定负载（建议沿用 Zootopia 1920×1080 / 5fps / ROI / 1271 帧，加测 Paddle 引擎与 1920×324 bottom-crop 两种几何）上产出 Native 阶段归因；按 `docs/design/ocr-performance-attribution.md` 第 5 节的决策表读数。
- **优先级**：P0（第一批的第一件事）。
- **风险**：低；纯测量，不改产品行为。
- **验收**：覆盖率 ≥99.9% 的归因报告 + 与 golden hash 对齐，版本化存入 `benchmark/baselines/`。

---

## 4. 优先级总表

| ID | 措施 | 维度 | 优先级 | 预期收益（量级） | 主要风险 | 验证门 |
|---|---|---|---|---|---|---|
| S0 | C++ Native 归因基线 | 测量 | P0 | 后续所有收益数字的对照系 | 无 | benchmark 版本化 |
| O1 | patrol 锚掩码缓存 | 算法 | P0 | changepoint −20~30% | 极低（数值等价） | CTest + golden |
| O2 | SSIM 缓冲复用 | 算法 | P0 | patrol 单次 −30~50% | 低（数值等价） | CTest + golden |
| O3 | 热路径去零填充 | 算法 | P0 | 每帧省 1 次全尺寸 memset；core wall −1~3% | 中低（未初始化内存 containment） | CTest + UBSan |
| O4 | det 前处理去冗余拷贝 | 算法 | P0 | 每次 OCR −2 次全帧 memcpy | 低 | Paddle parity 门 |
| O5 | SSE 索引化 + 有界 history | 缓存 | P0 | 长任务事件查询 O(N²)→O(k) | 低（cursor 契约） | Web + server 测试 |
| O6 | frame 模式进度节流 | 调度 | P0 | 消除每帧 UDS 事件风暴 | 低 | Swift 测试 |
| O7 | 二进制路径缓存 + 探测合并 | 调度 | P0 | 每 job −1 子进程 + 重复 PATH 扫描 | 低 | ffmpeg 契约测试 |
| O8 | 系统探测缓存 | 缓存 | P0 | 批量创建省 N−1 次探测 | 低 | server 测试 |
| O9 | 智能选区 ROI 化 | 算法 | P1 | 选区路径 −50%+ | 中（选区质量） | 选区 GT 对比 |
| O10 | stable/sample 克隆重构 | 算法 | P1 | 稳态每帧 −1 次全帧拷贝 | 中（所有权契约） | CTest + golden + RSS |
| O14 | 引擎跨 job 缓存 + warmup | 缓存/调度 | P1 | 批量每 job 省秒级模型加载 | 中高（内存/失效/GPU 设计张力） | ADR + 基准 + 失效用例 |
| O15 | 并发可配置化 + 准入 | 并发 | P1 | 批量吞吐近线性（需实测标定） | 中高（CPU 超订/持久化争用） | ADR + 契约 + 基准 |
| O16 | ORT 线程/arena 重评 | 并发 | P1 | 不确定 | 高（尾数/产品语义） | ADR + SHA exact 硬门 |
| O17 | 持久化写路径 | 调度 | P1 | 大 batch 全局停顿大幅下降 | 中（崩溃一致性） | 契约 + 崩溃恢复用例 |
| O18 | 取消异步化 | 调度 | P1 | 取消延迟降至标志级 | 中（取消契约/竞态） | IPC 契约测试 |
| O19 | 线程池修复 | 调度 | P1 | 阻塞端点与 SSE 不再争用 | 中低 | server 契约测试 |
| O11 | RGB→BGR+灰度融合 | 算法 | P2 | core wall −2~4% | 高（parity，或需重冻 golden） | 双路径 hash + SRT SHA |
| O13 | 跨段 OCR 记忆化 | 缓存 | P2 | 不确定（先测重复率） | 中（误命中） | 重复率测量先行 |
| O20 | ffmpeg `-threads`/hwaccel | 调度 | P2 | extract_wait 可能显著下降 | hwaccel 高（parity/回退语义） | A/B 基准 + golden |
| — | dHash/SSIM 降采样 | 算法 | 不做 | — | parity 锁定 | 记录防重提 |
| — | GPU 执行后端 | 并发/调度 | 长期 | 设计目标 ≥20% 端到端（Phase 15） | 硬件阻塞；fail-closed | Phase 15 独立门 |

---

## 5. 分阶段路线建议

**第一批（P0，预计为一个维护 Phase 的 2–3 个 Deliverable）**
S0 → O1/O2/O3/O4 → O5/O6/O7/O8。特征：不触碰数值通路、不需要新基线、不改变任何产品语义；验收以现有 golden + 契约测试 + 前后基准对比为主。预期合计效果（Vision ROI 场景估算）：changepoint 阶段 −20~30%、OCR 前处理搬运 −50%+（Paddle 路径）、server 端事件/持久化/创建延迟显著改善；core wall 整体预计 −5~10%（Paddle 路径可能更高，以 S0 实测为准）。

**第二批（P1，建议拆为两个 Phase）**
- Phase A（资源生命周期）：O14 引擎缓存 + warmup + O17 持久化 + O18 取消异步化 + O19 线程池。需要 ADR（缓存与 GPU gate 的协调、持久化崩溃语义）。
- Phase B（吞吐）：O15 并发 + O16 ORT 重评 + O9 选区 + O10 克隆重构。每项独立基准门；O16 不过 SHA exact 门就不改默认值。

**第三批（P2/长期）**
O11（parity 验证或重冻 golden 的决策）、O20（先出 parity 证据）、O13（先测重复率）；GPU（Phase 15 D02–D04）按硬件就绪情况独立推进，其 `OcrExecutionGate` 设计已是本方案 O14/O15 的兼容框架。

---

## 6. 通用约束与风险登记

1. **parity / golden 契约**：任何进入数值通路的改动，必须保持双路径结果 hash（当前 `b2d35c1e25f156e1`）与三来源 SRT SHA exact；不满足即回退或作为独立的"基线重冻"决策另行审批，不得作为优化的附带结果。
2. **ADR-0025 / ADR-0028 冻结项**：canonical ORT dylib SHA、intra_op=4、Rec batch=6、`source_output_sha256_exact` 硬门。微基准收益不得绕过产品语义门（Rec batch=1 的教训）。
3. **单 consumer 串行契约**：Pipeline 与 OCR 实例不得跨线程共享；所有并发设计以"每 job 独立实例 + 租约"为前提。
4. **Phase 4.1 负向证据**：不得以线程/队列优化冒充 OCR 加速（`ocr-performance-attribution.md` 第 5 节）；吞吐优化先归因到单消费者最大项。
5. **fail-closed**：优化不得引入静默降级路径（引擎切换、硬解回退、provider 降级都一样）。
6. **测量纪律**：同机、同负载、同版本；warmup ≥1 + measured ≥3 取 median；每个措施独立前后基准；诊断开销与产品 wall 分开测。
7. **内存基线**：Paddle 路径单 job RSS 约 1.7GB（HURDLES ADR-0025 证据）；O14/O15 的任何常驻设计必须先做内存预算与准入，再谈吞吐。
8. **本文与 Phase 状态的关系**：本文不登记 Phase、不改 `phases.json`；实施时按 AGENTS.md 以维护 Phase（2–8 个纵向 Deliverable）立项，Task 只进会话计划。

## 7. 实施结果与优先级修正（2026-10-20）

P0（S0 + O1–O8）已全部落地（Phase 16 D01–D03），结论是**数值等价成立、端到端收益不成立**。

| 项 | 实测结果（同机 / 同负载 / warmup=1 + measured=3，median） |
|---|---|
| S0 归因 | core_wall 122,114.0ms，coverage 99.95%；`ocr` 90.15%，其中 `det_infer` **84.35%** |
| O1 / O2 | `changepoint` 3,577.3 → 3,168.2ms（−11.4%，约 core_wall 0.34%） |
| O3 | `frame_materialize` 96.6 → 0.5ms（−99.5%，约 core_wall 0.08%） |
| O4 | `det_preprocess` 1,039.2 → 1,032.8ms（−0.6%，噪声内） |
| O5–O8 | SSE 复杂度、进度事件量、子进程/PATH 探测、能力探测次数改善；不承诺端到端吞吐 |
| 端到端 wall | 真实视频 137.15s → 136.21s（−0.7%，运行噪声内） |
| 等价性 | golden hash `b2d35c1e25f156e1` 不变；真实视频 6 次输出 SRT 逐字节相同；ROI 路径 348 行 SRT 相同 |

**读数修正**（覆盖第 1、4、5 节的估算）：

1. 第 1、4 节的优先级排序以 Python/Vision 结构为参照，对 C++ Native Release 不成立：
   `extract_wait` ≈ 0（解码在 ffmpeg 子进程并行）、大图搬运合计 ≈ 0.1%、`changepoint` 2.93%。
   P0 的理论天花板因此 <1%，实测与之一致。
2. 唯一的结构性上限是 `det_infer` 84.35%。**O9（减少 det 输入面积/次数）、O14/O16（推理层）
   与 GPU（Phase 15）是唯一能改变端到端的类别**；O5–O8 只在 server 延迟/事件维度有意义，
   不应计入"加速"叙事。
3. O11/O12/O13 的约束不变：前两者受 parity 锁定，O13 仍未测量重复率。
4. 后续任何加速提案，须先用 S0 同协议给出"目标阶段当前占比 + 本措施预计移动的毫秒数"；
   给不出就不立项。P0 措施不再重提——它们已经是现状的一部分或被明确判定为噪声级收益。

---

## 附录：证据索引

- 归因基线：`benchmark/baselines/performance-attribution-baseline.md`
- OCR 内部归因契约：`docs/design/ocr-performance-attribution.md`
- 负向并发证据：`docs/HURDLES.md`（"ROI 后 producer/consumer 重叠未产生稳定的用户可见吞吐收益"）
- ORT/线程/batch 冻结：`docs/HURDLES.md`（ADR-0025/0028 两条）、`docs/DECISIONS.md`
- GPU 路线与 gate：`docs/design/gpu-execution.md`
- 架构与契约：`docs/ARCHITECTURE.md`、`docs/cpp/native-architecture.md`、`docs/cpp/runtime-contract.md`、`docs/cpp/worker-ipc-contract.md`
- 代码证据：正文各 `file:line`（2026-10-19 对 `cpp/` 全树普查）
