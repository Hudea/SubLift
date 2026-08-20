"""SubLift CLI 入口。

产品提取入口是 Native `build/cpp/bin/sublift`。本模块保留 console 脚本以便
给出明确的 fail-closed 提示；不再转发 Native Worker，也不接受 `--runtime`
或 `SUBLIFT_RUNTIME` 作为产品运行时选择。Python 进程内 Pipeline 仍存在于
包内，供隔离工具使用，但不是产品 CLI。
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from sublift.models import SCRIPT_AUTO, SCRIPT_VALUES

_NATIVE_CLI_HINT = (
    "错误：产品提取入口是 Native CLI（build/cpp/bin/sublift）。\n"
    "Python 包不再转发 Native Worker，也不接受 --runtime 或 SUBLIFT_RUNTIME。\n"
    "请使用：./build/cpp/bin/sublift extract <video> -o output.srt"
)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="sublift",
        description="硬字幕提取工具：产品入口为 Native CLI；本 Python 入口已关闭。",
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
    del video, output, fps, confidence, engine, script
    print(_NATIVE_CLI_HINT, file=sys.stderr)
    sys.exit(2)


if __name__ == "__main__":
    main()
