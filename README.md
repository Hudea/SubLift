# SubLift

硬字幕（烧录字幕）提取工具——从视频画面中自动识别字幕，生成可编辑的 SRT 文件。

本地离线运行，隐私优先，视频与识别文本不离开本机。

## 特性

- **Apple Vision OCR**：默认中英双语识别（zh-Hans + en-US），通过 PyObjC 桥接
- **像素差异打轴**：双信号帧签名（前景占比 + dHash）+ 三态状态机，时间轴稳定
- **OCR 后置**：每段代表帧只调一次 OCR，避免逐帧调用的开销
- **模块化可插拔**：extractor / detector / ocr / export 均为 Protocol，可替换实现

## 环境要求

- macOS 13+（Apple Silicon 推荐；Vision 依赖）
- Python 3.12+
- ffmpeg（含 ffprobe）
- uv（包管理）

## 安装

```bash
git clone <repo>
cd SubLift
uv sync                    # 基础依赖
uv sync --extra vision     # Apple Vision OCR（macOS 可选依赖）
```

> Vision 未安装时，OCR 集成测试自动跳过，pipeline 可用 MockOcrEngine 跑闭环测试。

## 使用

```bash
uv run sublift extract <video> -o output.srt
uv run sublift extract clip.mkv --fps 5 -o out.srt
uv run sublift extract clip.mkv --engine mock -o out.srt   # 无 Vision 时跑流程
```

### CLI 参数

| 参数 | 默认值 | 说明 |
|---|---|---|
| `<video>` | 必填 | 输入视频路径 |
| `-o, --output` | output.srt | 输出字幕文件路径 |
| `--fps` | 5.0 | 帧采样率（推荐 5.0） |
| `--confidence` | 0.5 | OCR 置信度阈值，低于此值的文本置空 |
| `--engine` | vision | OCR 引擎（vision / mock） |

## 开发

```bash
./init.sh                     # 环境检查 + 验证基线
uv run pytest                 # 单元测试（112 passed + 4 skipped）
uv run pytest -m integration  # 集成测试（需 ffmpeg / Vision）
uv run ruff check .           # lint
uv run mypy src tests         # 类型检查（strict）
```

### 架构与设计

- [架构设计](docs/ARCHITECTURE.md) — 模块布局、数据流、分层原则
- [需求规格](docs/REQUIREMENTS.md) — 功能需求、非功能需求、验收标准
- 设计文档：[pipeline](docs/design/pipeline.md) · [ocr](docs/design/ocr.md) · [extractor](docs/design/extractor.md)
- [已知障碍](docs/HURDLES.md) — 开发中遇到的技术问题与解决方案

### 技术栈

Python 3.12+ / uv / Pillow / NumPy / OpenCV / PyObjC（Vision+Quartz）/ ffmpeg
