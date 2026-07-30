# phase4-roi-data-path 设计与执行计划

> 本文件是 Phase 4 的设计源头。任务状态与实际验证证据以
> [phase4.json](../phases/phase4.json) 为准；项目级功能块以
> [feature-list.json](../../feature-list.json) 为准。
>
> **状态：已开工，尚未实施 ROI 代码。** 本文描述目标架构与验收，不得把它当成现有行为说明。

## 1. 决策

Phase 4 固定为：

> **ROI 数据通路与真实长流验收**：对已知、有效的固定字幕区域，让 ffmpeg
> 在 RGB 输出前裁出字幕带，只把 ROI 像素交给 Python；保持固定 GT 的结果等价，
> 并用一段真实的非 Zootopia 长视频完成 GUI 体验验收。

这不是“只解码字幕区域”。H.264 / HEVC 等编码通常仍需要重建完整编码帧；
本阶段减少的是裁剪后的 RGB 转换、stdout 管道传输、Python 图像构造、PIL 复制和
后续冗余裁剪。

Phase 3 的默认 GUI 已是 path mode：Swift 只发送视频路径、固定 region 和
SubtitleProfile，Python 端边抽帧边调用 Pipeline.feed。因而当前主瓶颈不是
Swift 持续传 JPEG，也不是“伪流式”帧累计，而是 ffmpeg 到 Python 的完整 RGB
帧搬运。

## 2. 立项依据与性能锚点

feat-037 的 canonical baseline 是本阶段唯一的性能比较起点：

| 项 | 固定值 |
|---|---:|
| 视频 | debug/Zootopia_clip_1080p.mp4，约 254.27 秒 |
| GT | benchmark/datasets/Zootopia_clip_1080p_gt.srt，87 条 |
| 源尺寸 / fps | 1920 × 1080 / 5 |
| 固定 region | [0, 848, 1920, 87] |
| OCR / 文字系统 | Vision / cjk |
| 协议 | 1 次 warmup + 3 次独立 measured，取 median |
| core wall | 11,175.4 ms，22.75× 实时 |
| OCR | 约 4,544 ms，40.7% |
| extract_wait | 约 2,691 ms，24.1% |
| frame_materialize | 约 1,658 ms，14.8% |
| raw_output_bytes | 7,906,636,800 B，约 7.36 GiB |

固定 ROI 的面积与 raw RGB 输出占比为 87 / 1080 = 8.0556%。同样 1,271 帧时：

| 项 | 全帧 | ROI |
|---|---:|---:|
| 单帧 RGB 字节 | 6,220,800 B | 501,120 B |
| 总 stdout 原始字节 | 7,906,636,800 B | 636,923,520 B（理论值） |
| 相对全帧 | 100% | 8.0556% |

这保证 ROI 的第一收益可以被精确测量；不把 12.4× 像素传输缩减误写成 12.4×
端到端加速承诺。

## 3. 范围与非目标

### 3.1 本阶段范围

1. **固定区域 path mode**：GUI 和 benchmark 已有有效固定 region 时自动或显式走
   ffmpeg ROI 输出。
2. **同提交 A/B**：benchmark 可在内部强制 full 或 roi 输出，以同机器、同提交、
   同负载比较，而非只与历史数字比较。
3. **真实 GUI 长流验收**：用一段不入库的、非 Zootopia、至少 10 分钟硬字幕视频，
   验证真实交互、取消、重启和导出。

### 3.2 明确不做

- 不改 OCR、行选择、共识、SSIM、采样 fps、打轴阈值或 GT 质量策略。
- 不做 JPEG 重编码、缩放、Swift 端帧流重构、并发队列或 OCR 缓存。
- 不给 GUI 增加 ROI / 性能模式开关；有效固定选区是默认优化行为。
- 不扩展 CLI BottomCrop 到 ROI 输出；CLI 和 region_box 为空的路径保持全帧回退。
- 不实现 codec / 硬件级 ROI decode，不声称只解码字幕区域。
- 不把 Phase 4 的单个真实长视频体验结果写成跨片源质量泛化结论。
- 不在本阶段处理英文、中英混排、不同字幕位置的 GT 扩充；这些是 Phase 5 的质量泛化工作。

## 4. 目标架构与路由边界

详细坐标与接口契约见 [ROI 数据通路设计](../design/roi-data-path.md)。

~~~text
GUI / benchmark 的 source_region_box
  → ffprobe 校验源输出尺寸、区域与旋转兼容性
  → ffmpeg: fps=<fps>,crop=<w>:<h>:<x>:<y>:exact=1
  → 仅 ROI rgb24 写入 stdout
  → Frame.image（ROI 局部坐标）
  → RoiPassthroughDetector（[0, 0, w, h]）
  → Pipeline 直接使用整张 ROI 进行 signature / OCR
~~~

| 输入路径 | ffmpeg 输出 | Pipeline 区域语义 | Phase 4 行为 |
|---|---|---|---|
| path mode + 有效固定 region | ROI raw RGB | ROI 局部全幅 | 自动走 ROI |
| benchmark + 固定 region | full 或 roi（内部 A/B） | 对应全帧或 ROI 局部 | 两组均可跑 |
| path mode + region 为空 | 全帧 raw RGB | BottomCrop 原语义 | 保持不变 |
| Swift legacy frame mode | Swift 全帧 JPEG | 原有全帧语义 | 保持不变 |
| 非恒等旋转 / 坐标映射未验证 | 全帧 raw RGB | 原有全帧语义 | 安全回退并记录 |

显式 ROI 请求如果越界、零尺寸、ffmpeg crop 失败或输出尺寸不符，必须报任务错误；
禁止静默 clamp、裁黑边或退回全帧。这类退化会掩盖坐标 bug，破坏性能和质量可比性。

## 5. Feature 拆分与完成定义

### feat-038：固定区域 FFmpeg ROI 输出通路

**目标**：在不改变打轴/OCR 算法的条件下，为 GUI path mode 与 benchmark 的固定
region 建立 crop-before-Python 通路。

**依赖**：feat-037 性能模式与 canonical baseline。

**内部子任务**：

| 子任务 | 交付 |
|---|---|
| 038a 坐标与参数契约 | 区分 source-frame、frame-local、OCR-crop；校验 output_crop；明确旋转安全回退。 |
| 038b extractor 输出 | FfmpegExtractor 支持可选 output_crop，使用 fps 后 exact crop，按 ROI 尺寸读 raw RGB，并保留取消/错误语义。 |
| 038c Pipeline 局部消费 | 新增语义明确的 RoiPassthroughDetector；ROI 全幅直接透传，不把 source region 再用于 PIL.crop。 |
| 038d 路由与诊断 | bridge 默认固定 region 自动 ROI；benchmark 增加内部 full / roi A/B；性能报告记录输出模式与 source region。 |
| 038e 自动验证 | 补充单元、ffmpeg 合成视频、Pipeline、bridge、benchmark 回归测试。 |

**完成定义（全部必须满足）**：

1. output_crop 在启动 ffmpeg 前以探测尺寸严格校验：x、y 非负，w、h 正数，
   x+w ≤ source_width，y+h ≤ source_height；奇数坐标/尺寸允许且使用 exact=1。
2. ROI 每帧读取严格为 w × h × 3 字节；合成视频测试证明 ROI 的尺寸、像素、帧数与
   时间戳序列等于“全帧输出后裁同一区域”的结果。
3. ROI 输入 Pipeline 后，Region.box 必为 [0, 0, w, h]；source region 不得进入
   Pipeline 的图像裁剪路径；全幅 ROI 必须零拷贝透传或等价地不产生第二次 PIL crop。
4. SubtitleProfile、OcrLine 均继续使用 ROI / OCR 输入的局部坐标，不发生 Y 偏移。
5. GUI path mode 的有效固定 region 自动走 ROI；无 region、legacy frame mode、
   BottomCrop 和旋转未验证路径的既有行为不变。
6. ROI 取消仍会终止活跃 ffmpeg；worker 能回收，紧接着的第二任务可正常启动。
7. 性能报告至少包含 source/output 尺寸、source_region_box、output_mode、
   raw_bytes_per_frame、full_frame_passthrough_count 与 pipeline_crop_count。
8. Python ruff、mypy、pytest、init.sh，以及 Swift swift test 全绿。

### feat-039：ROI A/B 性能与固定 GT 回归

**目标**：以同提交 full 对照和 roi 实验组，证明固定 GT 结果等价且性能收益真实。

**依赖**：feat-038。

**执行协议**：

~~~text
同一 clean commit、同一台机器、同一 manifest：
  full：frame_output_mode=full，warmup=1，measured=3
  roi ：frame_output_mode=roi ，warmup=1，measured=3

video=debug/Zootopia_clip_1080p.mp4
GT=benchmark/datasets/Zootopia_clip_1080p_gt.srt
region=[0,848,1920,87]，fps=5，Vision，cjk，performance=summary
每次 measured 均为独立进程，主统计量为 median。
~~~

**硬验收门（任一失败即不收口）**：

| 类别 | 必须满足 |
|---|---|
| 采样等价 | full 与 roi 的 frame_count、时间戳序列完全一致。 |
| 像素传输 | ROI raw_bytes_per_frame = 501,120 B；同帧数下 raw_output_bytes 比例严格为 87/1080；当前 canonical 预期为 636,923,520 B。 |
| 结果等价 | ROI 的 detection_hash 与同次 full 对照完全一致；同环境下应同时匹配历史锚 b2d35c1e25f156e1。若环境变化致 full hash 改变，先重建并记录 full baseline，不能直接把 ROI 与旧值混比。 |
| 固定 GT 质量 | 三次 ROI measured 均满足 F1 ≥ 95.2%、precision ≥ 98.8%、usable ≥ 85.1%、CER macro ≤ 6.6%、noise ≤ 2、empty ≤ 1。 |
| 构造成本 | ROI 的 frame_materialize.total_ms 中位数 ≤ 同次 full 对照的 25%。 |
| 端到端非回退 | ROI core_wall_ms 中位数 ≤ 同次 full 对照的 105%；peak RSS 中位数 ≤ full 的 105%；stage_coverage_pct ≥ 99%。 |
| 报告完整 | A/B agent JSON、summary、质量 gates 与环境元数据归档到 debug/benchmark/archive/legacy-runs/feat039_roi_ab/；报告明确 full / roi 不是跨机器比较。 |

**期望但非阻断目标**：ROI core_wall 中位数 ≤ full 的 90%，realtime_factor ≥ full 的
1.10 倍。若未达到，仍可在硬门通过后完成 feat-039，但必须在证据中解释 OCR / codec
decode 等未受 ROI 改变的剩余成本；不得用 JPEG、缩放或算法改动补跑来混淆因果。

### feat-040：非 Zootopia 长视频 GUI 真实体验验收

**目标**：补齐自动测试无法替代的真实拖拽与长流体验覆盖。

**依赖**：feat-039。

**测试素材条件**：

- 本地真实硬字幕视频，非 Zootopia，时长至少 10 分钟；
- 在前 60 秒内至少出现一条字幕；
- 使用 GUI 默认 path mode 与实际候选框选出的固定字幕区域；
- 视频不提交仓库。证据只记录时长、分辨率、容器、字幕位置、日期和不可逆标识。

**硬验收门（必须留存截图、屏幕录制或带时间戳日志）**：

1. 完整跑完一次：状态只能按“就绪 → 启动/处理中 → 整理中 → 完成”前进；百分比
   单调不倒退；首条字幕在开始后 10 秒内出现；SRT 可导出并再次打开。
2. 处理至少推进 30 秒视频时间后点击取消：从点击到 UI 显示可再次提取的耗时 ≤ 1.0 秒。
3. 取消后 5 秒内重新启动同一视频：出现新的 processing 进度；无旧字幕残留、ffmpeg
   残留进程、socket 错误或界面卡死。
4. 全程无崩溃、内存压力警告或持续性 UI 卡顿；至少记录约处理 1 分钟与完成时的 Python
   RSS，以及完整任务的最高 RSS。
5. 该验收只证明体验与稳定性；不对该视频宣称 timing / CER / 混排质量结论。

## 6. 执行顺序与停机规则

~~~text
feat-037 baseline（已完成）
        ↓
feat-038 固定 ROI 输出机制
        ↓
feat-039 同提交 A/B + 固定 GT 收口
        ↓
feat-040 真实长视频 GUI 验收
        ↓
feat-041 性能计时归因收口
        ↓
Phase 4 文档收口
~~~

- feat-038 未满足像素/坐标不变量时，禁止进入性能调优。
- feat-039 的 detection_hash 不一致时，禁止以“质量门仍通过”收口。
- feat-040 不用 Python Mock 长流替代；没有真实 GUI 证据，Phase 4 保持 in-progress。
- 任一性能结论必须同附该次质量结果和环境指纹。
- feat-041 前，stage coverage 低于 99% 的 A/B 不用于解释端到端 wall 差异；不得把
  未归因时间直接归咎于 ROI 或 Vision。

### feat-041：性能计时归因收口

**目标**：补齐 `Pipeline.run_frames()` 叶子阶段之间的编排耗时，使 performance summary
能够解释 core wall，而不改变 ROI 输出、OCR、打轴或质量结果。

**设计**：新增 `pipeline_overhead` 排他 stage。它测量 `run_frames` 的完整 wall，再扣除
其内部互不重叠的 coverage leaf stages；因此帧迭代、状态机/Timeline 分派、容器阶段自身
开销与 recorder 固定成本有明确归属，同时不会与 `ocr`、`crop`、`dedupe` 等重复相加。
`finalize` 继续是容器 stage，不直接进入 coverage；其未嵌套的编排部分归入
`pipeline_overhead`。

**完成定义（全部必须满足）**：

1. fake-clock 测试证明 `pipeline_overhead + leaf stages = core wall`，且嵌套的
   `finalize` 不会造成双计；
2. 既有 off / summary 产出的字幕结果一致，ruff、mypy、pytest 与 `init.sh` 全绿；
3. canonical full / roi 各 warmup=1 + measured=3 的复验中，每次 ROI measured
   `stage_coverage_pct ≥ 99%`；
4. A/B 报告继续记录 detection hash、固定 GT 质量、raw bytes、RSS 与 core wall；若
   wall 结论仍受 Vision 方差影响，明确标为测量事实而非 ROI 的因果性能承诺。

## 7. Phase 4 完成定义

Phase 4 仅在以下条件同时满足时标记 done：

1. feat-038、feat-039、feat-040、feat-041 全部 done，且各自证据写入 phase4.json；
2. ROI 输出的坐标、像素、取消与兼容回退自动测试齐全；
3. canonical A/B 的硬性能与固定 GT 质量门全部通过；
4. 非 Zootopia ≥10 分钟 GUI 真实验收有可审计证据；
5. docs/ARCHITECTURE.md、ROI 设计、benchmark 文档、feature-list.json、progress.md 与
   DECISIONS.md 同步到实际实现；
6. Python init.sh 与 Swift swift test 在收口提交上通过。

## 8. 后续 Phase 5 候选（不提前混入）

Phase 4 的 A/B 结果出来后，再决定下一个机制优化或质量泛化方向。当前明确后置：

- 英文、中英混排、不同字幕位置、不同片源的 GT 扩充与泛化质量优化；
- CLI BottomCrop ROI、旋转视频坐标支持、动态字幕区域；
- 缩放、JPEG/其他像素格式、并发 pipeline、OCR 缓存；
- PaddleOCR、ASS/VTT、配置文件和批量队列。
