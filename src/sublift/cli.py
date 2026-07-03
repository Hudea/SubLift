"""SubLift CLI 入口。"""

import argparse


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
        "--fps", type=float, default=1.0, help="帧采样率（默认 1.0 fps）"
    )
    extract_parser.add_argument(
        "--confidence",
        type=float,
        default=0.5,
        help="OCR 置信度阈值（默认 0.5）",
    )
    return parser


def main(argv: list[str] | None = None) -> None:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.command is None:
        parser.print_help()
        return
    if args.command == "extract":
        parser.error(
            f"extract 命令尚未实现（视频: {args.video}, 输出: {args.output}）。"
            "pipeline 与 ocr 模块完成后将接入。"
        )


if __name__ == "__main__":
    main()
