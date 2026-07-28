"""SubLift CLI 入口。"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

from sublift.config import DEFAULT_CONFIG, Config
from sublift.detector import BottomCropDetector
from sublift.export import SrtExporter
from sublift.extractor import FfmpegExtractor
from sublift.models import SCRIPT_AUTO, SCRIPT_VALUES
from sublift.ocr import (
    MockOcrEngine,
    PaddleOcrEngine,
    VisionOcrEngine,
    is_paddle_available,
    is_vision_available,
)
from sublift.pipeline import Pipeline


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="sublift",
        description="硬字幕提取工具：从视频画面中识别烧录字幕并导出为字幕文件。",
    )
    subparsers = parser.add_subparsers(dest="command")

    extract_parser = subparsers.add_parser("extract", help="从视频提取硬字幕并导出为字幕文件")
    extract_parser.add_argument("video", help="输入视频文件路径")
    extract_parser.add_argument(
        "-o", "--output", default="output.srt", help="输出字幕文件路径（默认 output.srt）"
    )
    extract_parser.add_argument(
        "--fps",
        type=float,
        default=5.0,
        help="帧采样率（默认 5.0 fps）",
    )
    extract_parser.add_argument(
        "--confidence",
        type=float,
        default=0.5,
        help="OCR 置信度阈值（默认 0.5）",
    )
    extract_parser.add_argument(
        "--engine",
        choices=["vision", "mock", "paddle"],
        default="vision",
        help="OCR 引擎（默认 vision；mock 用于流程验证；paddle 跨平台）",
    )
    extract_parser.add_argument(
        "--script",
        choices=sorted(SCRIPT_VALUES),
        default=SCRIPT_AUTO,
        help="字幕文字系统（默认 auto；可选 cjk/latin）",
    )
    return parser


def main(argv: list[str] | None = None) -> None:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.command is None:
        parser.print_help()
        return
    if args.command == "extract":
        _run_extract(
            video=Path(args.video),
            output=Path(args.output),
            fps=args.fps,
            confidence=args.confidence,
            engine=args.engine,
            script=args.script,
        )


def _run_extract(
    *,
    video: Path,
    output: Path,
    fps: float,
    confidence: float,
    engine: str,
    script: str,
) -> None:
    """执行端到端字幕提取。"""
    if not video.exists():
        print(f"错误：视频文件不存在: {video}", file=sys.stderr)
        sys.exit(1)

    print("[1/3] 正在探测视频...")
    from sublift.extractor.ffmpeg_extractor import probe_video

    try:
        info = probe_video(video)
        width, height = info.width, info.height
        duration_ms = info.duration_ms
        print(f"  分辨率: {width}x{height}  时长: {duration_ms / 1000:.1f}s")
    except Exception as e:
        print(f"  警告：无法探测视频属性 ({e})")
        duration_ms = 0

    ocr = _build_ocr_engine(engine)
    extractor = FfmpegExtractor(fps=fps)
    detector = BottomCropDetector(bottom_ratio=DEFAULT_CONFIG.region_bottom_ratio)
    config = Config(
        sample_fps=fps,
        confidence_threshold=confidence,
        subtitle_script=script,
    )

    pipeline = Pipeline(detector=detector, ocr=ocr, config=config, extractor=extractor)

    print(f"提取字幕：{video}")
    print(f"采样率：{fps}fps  引擎：{engine}  置信度阈值：{confidence}  文字系统：{script}")

    start = time.perf_counter()
    est_total_frames = int(duration_ms / 1000 * fps) if duration_ms > 0 else 0

    if sys.stdout.isatty():
        sys.stdout.write("[2/3] 提取与识别: 准备中...\r")
        sys.stdout.flush()
    else:
        print("[2/3] 提取与识别: 开始运行...")

    frames = extractor.extract(video)

    last_reported_pct = -1

    for frame in frames:
        event = pipeline.feed(frame)
        if event is not None:
            pipeline.ocr_segment(event)

        processed = pipeline.processed_count
        if est_total_frames > 0:
            pct = min(processed / est_total_frames, 1.0)
            if sys.stdout.isatty():
                bar_len = 30
                filled = int(pct * bar_len)
                if filled < bar_len:
                    bar = "=" * filled + ">" + " " * (bar_len - filled - 1)
                else:
                    bar = "=" * bar_len
                sys.stdout.write(
                    f"\r[2/3] 提取与识别: [{bar}] "
                    f"{pct * 100:.1f}% ({processed}/{est_total_frames} 帧)"
                )
                sys.stdout.flush()
            else:
                pct_10 = int(pct * 10)
                if pct_10 > last_reported_pct:
                    last_reported_pct = pct_10
                    print(f"[2/3] 提取与识别: {pct_10 * 10}% ({processed}/{est_total_frames} 帧)")
        else:
            if sys.stdout.isatty():
                sys.stdout.write(f"\r[2/3] 提取与识别: 已处理 {processed} 帧")
                sys.stdout.flush()
            else:
                if processed % 100 == 0:
                    print(f"[2/3] 提取与识别: 已处理 {processed} 帧")

    if sys.stdout.isatty():
        sys.stdout.write("\n")
        sys.stdout.flush()

    sys.stdout.write("[3/3] 正在整理并导出字幕...\n")
    sys.stdout.flush()

    entries = pipeline.finalize()
    elapsed = time.perf_counter() - start

    SrtExporter().export(entries, output)

    print(f"完成：{len(entries)} 条字幕 → {output}（耗时 {elapsed:.1f}s）")


def _build_ocr_engine(
    engine: str,
) -> VisionOcrEngine | MockOcrEngine | PaddleOcrEngine:
    """构造 OCR 引擎。"""
    if engine == "vision":
        if not is_vision_available():
            print(
                "错误：Apple Vision 不可用。请安装 macOS 可选依赖：\n"
                "  uv sync --extra vision\n"
                "或使用 --engine mock 跑流程验证。",
                file=sys.stderr,
            )
            sys.exit(1)
        return VisionOcrEngine()
    if engine == "mock":
        return MockOcrEngine(text="[mock subtitle]", confidence=1.0)
    if engine == "paddle":
        if not is_paddle_available():
            print(
                "错误：PaddleOCR 不可用。请安装可选依赖：\n  uv sync --extra paddle",
                file=sys.stderr,
            )
            sys.exit(1)
        try:
            return PaddleOcrEngine()
        except Exception as exc:
            detail = str(exc) or type(exc).__name__
            print(
                "错误：PaddleOCR 初始化失败，无法加载或下载识别模型。\n"
                f"  原因：{detail}\n"
                "  请检查网络后重试；首次运行需要下载模型。\n"
                "  也可先预下载模型：\n"
                '  uv run --extra paddle python -c "from sublift.ocr import '
                'PaddleOcrEngine; PaddleOcrEngine()"',
                file=sys.stderr,
            )
            sys.exit(1)
    print(f"错误：未知引擎: {engine}", file=sys.stderr)
    sys.exit(1)


if __name__ == "__main__":
    main()
