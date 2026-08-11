# macOS GUI 模块设计

> `apps/macos/Sources/SubLiftMac/` — SwiftUI 壳、运行时路由、UDS 客户端、预览、编辑与导出。
>
> ADR-0005 记录了最初的 SwiftUI + Python Worker 方案；Phase 6 cutover 后，GUI 默认启动
> C++ `sublift_worker`，Python server 只在显式 runtime 下承担 Oracle / 开发回滚。

## 运行时边界

GUI 不在 Swift 进程中执行字幕 Pipeline。`RuntimePolicy` 先解析 runtime 与 engine，
`PipelineClient` 再启动对应 Worker；两种 Worker 使用同一套 UDS framing 与业务消息。

| 选择 | 后端 | 行为 |
|---|---|---|
| 默认 vision / mock | C++ `sublift_worker` | 产品主路径 |
| 默认 paddle，capability 可用 | C++ `sublift_worker` | 产品主路径 |
| 默认 paddle，capability 不可用 | 无 | fail-closed，显示可操作错误 |
| 显式 `runtime=python` | `python -m sublift.ipc.server` | Oracle / 开发回滚 |

解析优先级为显式参数 → `SUBLIFT_RUNTIME` → 产品默认 C++。不允许因 C++ capability
缺失而静默切换 Python、Vision 或 Mock。GUI 记录并展示最终 `WorkerChoice`，避免界面设置
与真实执行后端漂移。

## 模块职责

### Core

| 文件 | 职责 |
|---|---|
| `RuntimePolicy.swift` | 解析 runtime / engine / 来源；执行 C++ 默认与 fail-closed 规则。 |
| `PipelineClient.swift` | 查找并启动 C++ 或显式 Python Worker；负责 UDS 连接、framing、握手、请求与资源回收。 |
| `Messages.swift` | IPC Codable 消息；字段与 C++/Python 共享协议对齐。 |
| `SubtitleExtractor.swift` | 发送 path-mode `start_job`，消费 progress / push_entry / entries / done，协调取消、日志与处理倍速。 |
| `FrameSampler.swift` | 为预览候选框扫描、代表帧和 legacy frame mode 提供 AVFoundation 采样；不参与默认打轴。 |
| `FfmpegFallback.swift` | 为 mkv 预览/代表帧、元数据及 legacy frame mode 提供系统 ffmpeg 兜底。 |
| `VideoMetadata.swift` | 异步读取文件名、大小、分辨率、时长和编码；mkv 使用 ffprobe 兜底。 |
| `VisionTextDetector.swift` | 在 GUI 内对代表帧检测文字候选框；只服务选区，不承担产品 OCR。 |
| `RegionSelectionModel.swift` | 管理候选框多选，并推算全宽字幕带区域。 |
| `VideoCoordinateMapper.swift` | 转换像素、视图与 Vision 归一化坐标。 |
| `SubtitleEditor.swift` / `SubtitleEntry.swift` | 维护可编辑字幕、当前高亮、合并与拆分。 |
| `SrtFormatter.swift` | 在 Swift 端格式化 SRT，不调用 Worker。 |
| `ProcessingRate.swift` / `PreviewLayout.swift` | 处理倍速格式化与预览布局纯计算。 |

### UI

| 文件 | 职责 |
|---|---|
| `SubLiftMacApp.swift` | `@main` 入口、主视图组合与应用状态。 |
| `DropZone.swift` | 接收本地视频拖入。 |
| `VideoPreview.swift` | AVPlayer 预览、播放/暂停与 seek。 |
| `RegionOverlay.swift` | 候选框叠加、多选与合并区域预览。 |
| `SubtitleList.swift` | 字幕列表、文本编辑、合并/拆分和导出。 |
| `SettingsView.swift` | vision / paddle / mock 选择与 UserDefaults 持久化。 |
| `TimeFormatter.swift` | 毫秒时间格式化。 |

## IPC 协议

### 传输与进程

- **传输**：Unix Domain Socket；Swift 为每次连接生成临时 socket 路径。
- **分帧**：4 字节大端 body 长度 + UTF-8 JSON。
- **C++ 启动**：`sublift_worker --socket <path> --engine <engine>`。
- **Python 启动**：`python -m sublift.ipc.server --socket <path> --engine <engine> --log-level INFO`，仅显式回滚。
- **关闭**：`PipelineClient.stop()` 幂等关闭 fd、终止子进程并 unlink socket。
- **能力探测**：Paddle 路由前对将要启动的 C++ Worker 执行 `--probe-engine paddle`。

### 消息类型

| 方向 | 消息 | 关键语义 |
|---|---|---|
| Swift → Worker | `hello` / `bye` | 能力握手与连接关闭。 |
| Swift → Worker | `start_job` | `video_id`、`video_path?`、fps、engine、region、subtitle profile；有 `video_path` 即 path mode。 |
| Swift → Worker | `cancel_job` | 取消当前作业并释放 extractor / Pipeline 资源。 |
| Swift → Worker | `frame` / `finalize` | 仅 legacy frame mode；产品 GUI 不使用。 |
| Worker → Swift | `progress` | ready / processing / finalizing 阶段与 `pct`。 |
| Worker → Swift | `push_entry` | 字幕段闭合并完成 OCR 后的增量条目。 |
| Worker → Swift | `entries` | 最终 dedupe 后的全量结果，`is_final=true`。 |
| Worker → Swift | `log` / `done` / `error` | 诊断、业务结束与协议错误。 |

Worker 启动时绑定的 engine 是实际执行身份；若与 `start_job.engine` 不一致，返回明确失败，
不得以其他引擎继续。单条消息上限为 64 MiB；C++ 与 Python Worker 均遵守同一 framing
边界和字段兼容契约。

## 默认 GUI 数据流

```text
用户拖入视频
  ↓
DropZone → VideoMetadata → AVPlayer 预览
  ↓
VisionTextDetector 扫描代表帧 → 用户选择字幕区域
  ↓
RuntimePolicy.resolve(engine, runtime)
  ↓
PipelineClient 启动 C++ Worker（或显式 Python Worker）并握手
  ↓
start_job(video_path, region_box, subtitle_profile)
  ↓
Worker-owned FfmpegExtractor → Pipeline.feed / ocr_segment / finalize
  ↓
progress + push_entry → SubtitleEditor 增量展示
  ↓
entries(is_final=true) → SubtitleEditor 覆盖为最终去重结果
  ↓
SubtitleList 编辑 → SrtFormatter → NSSavePanel 写文件
```

默认 path mode 不发送 JPEG `frame`，避免 AVFoundation 跳采样相位与有损编码造成质量漂移。
C++ 与 Python extractor 是两套实现，其时间戳、ROI、像素和取消行为由冻结 parity/golden
契约约束。

## 预览、选区与 legacy frame mode

- AVFoundation 负责播放预览和常见容器代表帧。
- 系统 ffmpeg 负责 mkv 预览/元数据兜底；缺失时 UI 提示安装。
- `VisionTextDetector` 只决定 `region_box`，不替代 Worker 内 OCR。
- 无 `video_path` 时协议仍支持 Swift 推 JPEG 的 legacy frame mode，供兼容测试和诊断；
  产品 GUI 始终发送视频路径并使用 Worker-owned path mode。
- legacy frame mode 继续限制 JPEG 大小、解码后像素数并拒绝无效 Base64/JPEG。

## 字幕区域与坐标

`RegionSelectionModel` 默认选择画面下部文字候选。多个候选合并为 source-frame 坐标：

```text
x = 0
width = video_width
y = min(selected.y) - padding
height = max(selected.maxY) - min(selected.y) + 2 * padding
```

`region_box` 随 `start_job` 一次发送。有效固定区域进入 ROI path；未选择区域时 Worker
使用 BottomCrop 语义。GUI 视图坐标、Vision 归一化坐标与 source-frame 像素坐标只在
`VideoCoordinateMapper` 中转换。

## 字幕编辑与导出

`SubtitleEditor` 在主线程维护带 UUID 的可编辑条目：

| 操作 | 行为 |
|---|---|
| load | 以 Worker 最终 entries 替换增量列表。 |
| updateText | 修改单条文本。 |
| merge | 与下一条合并时间范围和文本。 |
| split | 在时间中点拆分，后段文本置空。 |
| updateCurrent | 根据播放时间节流更新当前条目。 |

编辑后的导出完全在 Swift 端完成。`SrtFormatter` 校验非负时间和 `end >= start`，输出
`HH:MM:SS,mmm`、条目间空行及末尾换行。

## 已知约束

- 当前只支持 SwiftPM 开发者运行；独立 `.app`、依赖随包、签名和公证后置。
- Python runtime 是显式开发回滚，不代表未来发布 artifact 会携带 Python。
- legacy frame mode 仍是协议兼容面，移除前必须先审计测试与外部消费者。
- 一次只处理一个视频；批量队列和精细时间码拖动尚未实现。
- 长视频交互已有历史手工验收，但尚未形成持续运行的跨片源 GUI 回归套件。
