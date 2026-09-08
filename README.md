# SubLift

本地硬字幕提取工具：从视频画面识别烧录字幕，导出可编辑的 SRT。

视频和识别文本留在你的机器上，不上传。当前版本是 **0.1 技术预览**——能用，但不是应用商店安装包，也不是公网服务。

## 不是什么

- 不是已签名、已公证的 macOS `.app`
- 不是公网 HTTPS / 账号 / 反向代理
- 不含 GPU/CUDA 加速
- 不导出 ASS / VTT
- 不提取软字幕轨，也不做翻译

## 三个入口

| 入口 | 适合 | 引擎 |
|---|---|---|
| 局域网 Web | 在 Linux 服务器上用浏览器处理视频 | CPU 上的 PaddleOCR |
| Native CLI | 本机脚本、单文件或批量 | macOS 可用 Vision 或 Paddle；需自行编译 |
| macOS GUI | 本机拖拽、预览、校对 | 开发者构建（`swift run`），不是独立安装包 |

缺少模型、Worker 或所请求的引擎时会明确失败，不会静默换成别的引擎或 Python。

## 局域网 Web（Docker）

把 Web 工作台和识别服务跑在**可信局域网**的一台 Linux 机器上。媒体只通过宿主目录挂进容器 `/media`，镜像里不带你的视频。推理使用 CPU 上的 ONNX Runtime + PP-OCRv6。

需要 Docker Engine 24+。

```bash
mkdir -p /srv/sublift/media          # 把视频放到这个目录
cp .env.example .env                 # 可选；.env 不入库
# 编辑 SUBLIFT_MEDIA_DIR 为上面的绝对路径

SUBLIFT_MEDIA_DIR=/srv/sublift/media docker compose up -d --build
```

浏览器打开 `http://<服务器局域网IP>:8080`。

| 项 | 说明 |
|---|---|
| 媒体 | 宿主 `SUBLIFT_MEDIA_DIR` → 容器 `/media`，唯一可访问的视频根 |
| 端口 | 默认宿主 8080（`SUBLIFT_HTTP_PORT`） |
| 缓存与导出 | 写在媒体目录下的 `.sublift_cache/` |
| 更新 | 在仓库根执行 `docker compose build --no-cache && docker compose up -d` |

**不要把端口暴露到公网。** 也不要给浏览器工作台设置 `SUBLIFT_ACCESS_TOKEN`：当前网页不会带这个头，打开后会 401。该 token 只给脚本 / API 用。

排障：`docker compose logs -f`。若提示 Paddle 不可用，重新构建镜像，不要用空的 `./models` 目录盖住内置模型。

## 本机 CLI（macOS）

需要：macOS 13+、ffmpeg（含 ffprobe）、CMake 3.20+、C++20 工具链（推荐 Ninja）。

### Vision（macOS 默认）

```bash
cmake -S cpp -B build/cpp -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DSUBLIFT_ENABLE_VISION=ON
cmake --build build/cpp

./build/cpp/bin/sublift extract clip.mkv -o output.srt
./build/cpp/bin/sublift extract clip.mkv --fps 5 --script cjk -o out.srt
```

### Paddle

需要 OpenCV，以及 `resources/manifest.json` 约束的 ONNX Runtime（macOS 可用 `brew install onnxruntime`，1.28.0）。CMake 会拒绝 Python wheel 里的 ORT。

```bash
cmake -S cpp -B build/cpp-rel -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DSUBLIFT_ENABLE_OPENCV=ON -DSUBLIFT_REQUIRE_OPENCV=ON \
  -DSUBLIFT_ENABLE_PADDLE=ON -DSUBLIFT_REQUIRE_PADDLE=ON
cmake --build build/cpp-rel
./build/cpp-rel/bin/sublift resources install   # 首次下载并校验模型；之后可离线

./build/cpp-rel/bin/sublift extract clip.mkv --engine paddle -o out.srt
```

### CLI 参数

| 参数 | 默认 | 说明 |
|---|---|---|
| `<video>` | 必填 | 输入视频 |
| `-o, --output` | output.srt | 输出 SRT |
| `--fps` | 5.0 | 帧采样率 |
| `--confidence` | 0.5 | OCR 高置信门 |
| `--engine` | vision | `vision` / `paddle` / `mock` |
| `--script` | auto | `auto` / `cjk` / `latin` |

## macOS GUI

通过 SwiftPM 在开发者环境运行，**没有独立 `.app` 安装包**。处理 mkv 需要系统 ffmpeg。

```bash
cd apps/macos
swift build
swift run SubLiftMac
```

可以拖入 mp4 / mov / mkv，预览视频，选择字幕区域，看增量结果，编辑后导出 SRT。批量处理在独立的 Task Center（⌘⇧T）。

## 本机浏览器（loopback）

仅本机访问时，也可以不经过 Docker：

```bash
cmake --build build/cpp --target sublift_server
npm --prefix apps/web run build
./build/cpp/bin/sublift_server --static-dir apps/web/dist
# http://127.0.0.1:8080
```

## 许可证

源码为 [MIT](LICENSE)。PaddleOCR 算法来源、模型与其它第三方声明见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

## 开发

构建、测试、验证门和项目架构见 [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)。变更记录见 [CHANGELOG.md](CHANGELOG.md)。
