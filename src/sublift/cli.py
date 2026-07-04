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
from sublift.ocr import MockOcrEngine, VisionOcrEngine, is_vision_available
from sublift.pipeline import Pipeline


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="sublift",
        description="硬字幕提取工具：从视频画面中识别烧录字幕并导出为字幕文件。",
    )
    subparsers = parser.add_subparsers(dest="command")

    extract_parser = subparsers.add_parser(
        "extract", help="从视频提取硬字幕并导出为字幕文件"
    )
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
        choices=["vision", "mock"],
        default="vision",
        help="OCR 引擎（默认 vision；mock 用于无 Vision 环境的流程验证）",
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
        )


def _run_extract(
    *,
    video: Path,
    output: Path,
    fps: float,
    confidence: float,
    engine: str,
) -> None:
    """执行端到端字幕提取。"""
    if not video.exists():
        print(f"错误：视频文件不存在: {video}", file=sys.stderr)
        sys.exit(1)

    ocr = _build_ocr_engine(engine)
    extractor = FfmpegExtractor(fps=fps)
    detector = BottomCropDetector(
        bottom_ratio=DEFAULT_CONFIG.region_bottom_ratio
    )
    config = Config(
        sample_fps=fps,
        confidence_threshold=confidence,
    )

    pipeline = Pipeline(extractor, detector, ocr, config)

    print(f"提取字幕：{video}")
    print(f"采样率：{fps}fps  引擎：{engine}  置信度阈值：{confidence}")
    start = time.perf_counter()
    entries = pipeline.run(video)
    elapsed = time.perf_counter() - start

    SrtExporter().export(entries, output)

    print(f"完成：{len(entries)} 条字幕 → {output}（耗时 {elapsed:.1f}s）")


def _build_ocr_engine(engine: str) -> VisionOcrEngine | MockOcrEngine:
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
    print(f"错误：未知引擎: {engine}", file=sys.stderr)
    sys.exit(1)


if __name__ == "__main__":
    main()
