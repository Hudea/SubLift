# Phase 12 架构设计：跨平台 Web UI 与 C++ 原生服务

## 1. 架构目标与定位

Phase 12 的目标是在保持 C++ Core 核心提取算法与性能不变的前提下，为 Linux 服务器、NAS、Docker 容器以及跨平台无头环境提供 **原生 C++ Web 服务与浏览器端工作台（Web Workbench）**。

首期（MVP）以 **Phase 10 单视频工作台（Native Workbench）** 为交互基准，实现完整的视频导入、播放、Canvas ROI 拖拽框选、SSE 实时流式打轴出字、双向音画联动、行内字幕编辑与 SRT 导出。

---

## 2. 系统分层与技术选型

```text
[ 浏览器前端 (apps/web) ]
  ├── 播放器组件 (VideoPlayer.vue): HTML5 <video> + 10Hz 时码同步
  ├── 选区组件 (RoiOverlay.vue): Canvas 覆盖层 + 归一化坐标转换
  ├── 字幕组件 (LiveTranscript.vue): 虚拟列表 + 实时 SSE 接收 + 双向 Seek
  └── 设置组件 (SettingsBar.vue): 引擎自适应与采样配置
          │  ▲
          │  │ HTTP 206 视频流 / REST API
          │  │ Server-Sent Events (SSE) 实时事件流
          ▼  │
[ C++ 原生服务 (cpp/src/server) ]
  ├── HTTP / SSE 服务器 (HttpServer): 基于 cpp-httplib (单头文件, 0 外部依赖)
  ├── 任务生命周期与调度器 (JobManager)
  └── 核心流水线驱动 (BridgeHandler): 进程内直接调用 Pipeline / Extractor / OCR
```

### 关键设计决策
1. **0 Python 运行时依赖**：服务端直接采用现代 C++20 编写，输出单个无外部依赖的二进制文件 `sublift_server`。
2. **0 IPC 跨进程开销**：C++ Web 服务直接在内部线程调用 `BridgeHandler` / `Pipeline`，消除 UDS Socket 跨进程中转与序列化开销。
3. **HTTP-Native 实时通信**：采用标准 **Server-Sent Events (SSE)** 实现服务端向浏览器的流式事件下发（`progress`, `push_entry`, `done`），浏览器原生 `EventSource` 直连，支持自动断线重连。

---

## 3. Web API 契约设计

### 3.1 REST API

| 路径 | 方法 | 职责 | 请求体 / 参数 | 返回格式 |
|---|---|---|---|---|
| `/api/system/info` | `GET` | 查询系统支持的 OCR 引擎与版本 | 无 | `{"runtime":"cpp","engines":["vision","paddle"],"version":"0.1.0"}` |
| `/api/video/stream` | `GET` | 视频流式传输（支持 Range Header） | `path` (视频绝对路径) | `video/mp4` (HTTP 206 Partial Content) |
| `/api/video/frame` | `GET` | 快速截取指定时间点的代表帧 | `path`, `time_s` (可选，默认0) | `image/jpeg` |
| `/api/jobs` | `POST` | 创建并启动提取任务 | JSON 配置（见下） | `{"job_id":"job-uuid","status":"started"}` |
| `/api/jobs/{id}/cancel` | `POST` | 取消正在执行的任务 | 无 | `{"job_id":"job-uuid","status":"cancelled"}` |
| `/api/jobs/{id}/export` | `GET` | 导出字幕文件 | `format=srt` | `text/plain; charset=utf-8` (Content-Disposition) |

#### 创建任务请求格式 (`POST /api/jobs`)：
```json
{
  "video_path": "/path/to/video.mp4",
  "engine": "paddle",
  "fps": 2.0,
  "confidence_threshold": 0.5,
  "region_box": {
    "x": 0.0,
    "y": 0.7,
    "width": 1.0,
    "height": 0.3
  }
}
```

### 3.2 SSE (Server-Sent Events) 事件流 (`GET /api/jobs/{id}/events`)

客户端通过 `new EventSource('/api/jobs/' + jobId + '/events')` 监听服务端事件：

1. **`event: progress`**
   ```json
   {"job_id":"job-uuid","stage":"processing","pct":0.45,"fps":12.5,"eta_ms":15000}
   ```
2. **`event: push_entry`**
   ```json
   {
     "job_id":"job-uuid",
     "entry": {
       "index": 1,
       "start_ms": 1200,
       "end_ms": 3400,
       "text": "这是一条提取出的字幕内容",
       "confidence": 0.94
     }
   }
   ```
3. **`event: done`**
   ```json
   {"job_id":"job-uuid","ok":true,"total_entries":150,"elapsed_ms":8450}
   ```
4. **`event: error`**
   ```json
   {"job_id":"job-uuid","error":"视频文件无法读取"}
   ```

---

## 4. 前端组件与交互规范 (`apps/web`)

### 4.1 核心状态机 (WorkbenchState)
* `empty`：无视频载入态（支持拖入或路径输入）。
* `ready`：视频就绪态，播放器可交互，ROI 选区可用。
* `processing`：提取中，右侧字幕表实时接收 `push_entry`，控制栏进入只读保护并展示取消按钮与进度。
* `review`：提取完成，进入可编辑态（行内修改字幕、双向音画联动 Seek）。
* `failed` / `cancelled`：异常与取消态。

### 4.2 ROI 坐标映射 (Coordinate Mapping)
* 视口画布尺寸 `(viewWidth, viewHeight)` 与视频原始像素尺寸 `(videoWidth, videoHeight)` 存在 Letterboxing/黑边。
* 选区在前端统一转换为归一化矩形：
  $$\text{norm\_x} = \frac{x - \text{offset\_x}}{\text{render\_w}},\quad \text{norm\_y} = \frac{y - \text{offset\_y}}{\text{render\_h}}$$
* 提交给后端的 `region_box` 均为 `[0.0, 1.0]` 范围的浮点比例。

---

## 5. 跨平台支持矩阵

| 平台 | 提取引擎 | 视频解码 | 部署形态 |
|---|---|---|---|
| **macOS (开发/桌面)** | Apple Vision / PaddleOCR | FFmpeg / AVFoundation | 本地命令行 `sublift server` |
| **Linux x86_64 (CPU)** | PaddleOCR (ONNX Runtime AVX2) | FFmpeg | 单二进制 `sublift_server` / Docker |
| **Linux NVIDIA GPU** | PaddleOCR (ONNX Runtime CUDA) | FFmpeg (NVDEC 可选) | Docker 镜像 `sublift:cuda` |
