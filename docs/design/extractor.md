# 帧采样设计

> `src/sublift/extractor/` — 视频帧采样抽象与 ffmpeg 实现。

## 模块职责

| 文件 | 职责 |
|---|---|
| `base.py` | `Extractor` Protocol 定义 |
| `ffmpeg_extractor.py` | `FfmpegExtractor`，通过 subprocess 调 ffmpeg/ffprobe |

## Extractor Protocol

```python
@runtime_checkable
class Extractor(Protocol):
    def extract(self, video_path: Path) -> Iterator[Frame]: ...
```

`Frame(timestamp_ms: int, image: PIL.Image.Image)` — 带毫秒时间戳的帧，`image` 为 PIL.Image 中立表示。

`@runtime_checkable` 支持 `isinstance` 静态判定。

## FfmpegExtractor

通过 subprocess 调用系统 ffmpeg/ffprobe，按配置 fps 抽帧。

### 两阶段流程

```
阶段 1: ffprobe 探测分辨率
  ffprobe -v error -select_streams v:0
          -show_entries stream=width,height -of json <video>
  → (width, height)

阶段 2: ffmpeg rawvideo pipe 抽帧
  ffmpeg -v error -i <video>
         -vf fps=<fps>
         -f image2pipe -pix_fmt rgb24 -vcodec rawvideo -
  → stdout 流式 raw bytes
```

### 帧时间戳计算

```
timestamp_ms = int(frame_index / fps * 1000)
```

帧索引从 0 开始，时间戳为毫秒整数。精度取决于 fps：5fps → 200ms 粒度。

### 内存策略

流式迭代，不缓冲全部帧：

- `subprocess.Popen` 启动 ffmpeg 进程，stdout 为 pipe
- 每次从 stdout `read(frame_size)` 读取一帧的 raw bytes（`width * height * 3`，RGB24）
- `Image.frombytes("RGB", (width, height), raw)` 转为 PIL.Image
- `yield Frame` 后继续读下一帧，直到 `len(raw) < frame_size` 表示流结束
- 最后 `proc.wait()` 检查退出码，非 0 抛 `RuntimeError` 含 stderr

### 初始化参数

```python
FfmpegExtractor(fps: float = 1.0)
```

`fps` 为采样率（每秒抽帧数）。Pipeline 注入时用 `Config.sample_fps`（默认 5.0）。

## 系统依赖

- `ffmpeg` 必须在 PATH
- `ffprobe` 必须在 PATH（通常与 ffmpeg 一起安装）
- 支持主流容器与编码（mp4 / mkv / mov，H.264 / H.265），取决于系统 ffmpeg 编译选项

`./scripts/verify-standard.sh` 会检出 `ffmpeg` 是否可用；Harness L0 `./init.sh` 只检查协作文件与 Phase 索引，不检查主机工具链。

## 错误处理

| 情况 | 行为 |
|---|---|
| 视频文件不存在 | `FileNotFoundError` |
| ffprobe 失败 / 无视频流 | `RuntimeError` |
| ffmpeg 退出码非 0 | `RuntimeError` 含 stderr 内容 |

## 已知约束

- 时间戳精度受 fps 限制（5fps → 200ms 粒度，1fps → 1000ms 粒度）
- 不抽关键帧，按固定 fps 均匀采样
- 帧索引与时间戳是线性映射，不依赖视频实际帧率
