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
- **状态**：部分缓解（feat-031b SSIM patrol 已实现，merged_into_neighbor FN 减少约 70%）
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
- **状态**：未解决（feat-031 初步优化已收尾，留作后续迭代）
- **关联 feature**：feat-031（打轴检测优化）
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
- **待评估方案**（后续迭代）：
  1. **patrol 阈值调优**：`ssim_patrol_threshold` 从 0.92 调到 0.88~0.90，降低敏感度减少过切分
  2. **dedupe 模糊合并**：归一化后做编辑距离比较，距离/长度 < 比例（如 0.2）时合并，解决标点/噪声差异
  3. **短字幕专用路径**：`hysteresis_frames=1` + 降低 `presence_threshold`，但需配合 patrol 阈值调优避免 FP 增长
- **影响评估**：过切分使 precision 从潜在 94.9% 降到 91.1%（-3.8pp），但不影响 recall；短字幕漏检贡献了主要 FN（8/15 no_overlap）。两者均为 feat-031 后续迭代方向，不影响当前优化成果的可用性。
- **相关文件**：`src/sublift/pipeline/changepoint.py`（patrol 阈值）、`src/sublift/pipeline/dedupe.py`（`_merge_adjacent` 归一化）、`src/sublift/config.py`（`ChangePointConfig`）、`debug/Zootopia_clip_1080p_优化 3.srt`（验证产物）

---

### bridge 批量缓冲全帧导致内存随视频时长线性增长
- **日期**：2026-07-06
- **状态**：待解决（feat-029 增量处理架构的前置障碍）
- **关联 feature**：feat-029（增量处理架构）
- **现象**：GUI 提取长视频时内存占用随帧数线性增长，2 分钟 1080p clip ~3.6GB，10 分钟 ~18GB，长视频必 OOM。`bridge.py` 用 `MAX_FRAMES=60000` / `MAX_TOTAL_PIXELS` 硬上限兜底挡崩，但这是权宜之计，非正常设计。
- **排查路径**：
  1. 读 `bridge.py` `_handle_frame`：每帧解码成 PIL.Image 后 `self._frames.append(Frame(...))`，全部驻留内存
  2. 读 `_handle_finalize`：`await asyncio.to_thread(self._pipeline.run_frames, iter(self._frames))` —— 把攒完的列表包装成迭代器一次性交给 Pipeline
  3. 读 `core.py` `run_frames`：Pipeline 本身接收 `Iterator[Frame]`，内部逐帧过 changepoint → timeline，流式能力本就具备
  4. 读 `core.py` 内部缓存：`anchor_frames` dict 只存 IN/CHANGE 事件时刻的代表帧，数量 ≈ 字幕段数（几十到几百），可忽略
- **根本原因**：bridge 把本可流式的 Pipeline 退化成"先攒完再批量处理"模式。Pipeline 的 `run_frames` 接收 `Iterator[Frame]` 本来支持逐帧推进，但 bridge 硬把帧先攒成 `list[Frame]` 再 `iter(list)` 交出去，导致：
  - 内存随视频时长线性增长（每帧 1080p RGB ~6MB × 帧数）
  - 首条字幕要等全片处理完才出现（finalize 后才返回 entries）
  - 长视频必然 OOM，硬上限是兜底而非解决方案
- **解决方案方向**（feat-029 待设计）：
  1. **bridge 边收帧边推进 Pipeline**：`_handle_frame` 收到帧后立即喂给 Pipeline，不攒列表
  2. **打轴流式 + OCR 段闭合触发**：changepoint/timeline 本就是逐帧的；OCR 在段闭合（OUT/CHANGE 事件）时立即调用，产出该条字幕并 push 给 Swift
  3. **Pipeline 改 push 模型**：从 `run_frames(iterator) -> list[entries]` 改为 `feed(frame) -> entry | None`（或回调），每帧返回新闭合的段
- **影响评估**：当前批量模式对短 clip（<2 分钟）凑合可用，但作为产品形态不合格。feat-029 的核心就是把 bridge 的批量缓冲改掉。
- **相关文件**：`src/sublift/ipc/bridge.py`（`_handle_frame` / `_handle_finalize`）、`src/sublift/pipeline/core.py`（`run_frames`）、`src/sublift/pipeline/changepoint.py`（状态机本就逐帧）
