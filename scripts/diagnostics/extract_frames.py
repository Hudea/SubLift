"""从指定视频抽取有限帧，供人工检查抽帧结果。"""

from __future__ import annotations

import argparse
from pathlib import Path

from sublift.extractor import FfmpegExtractor


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="抽取指定视频的有限帧用于人工诊断")
    parser.add_argument("--video", type=Path, required=True, help="本地视频路径")
    parser.add_argument("--out", type=Path, required=True, help="输出目录")
    parser.add_argument("--fps", type=float, default=1.0)
    parser.add_argument("--limit", type=int, default=30, help="最多抽取的帧数")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.fps <= 0 or args.limit <= 0:
        print("--fps 与 --limit 必须大于 0")
        return 2

    video = args.video.expanduser().resolve()
    if not video.is_file():
        print(f"视频不存在: {video}")
        return 2

    out_dir = args.out.expanduser().resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    extractor = FfmpegExtractor(fps=args.fps)
    count = 0
    for frame in extractor.extract(video):
        if count >= args.limit:
            break
        path = out_dir / f"frame_{frame.timestamp_ms:06d}.png"
        frame.image.save(path)
        count += 1

    print(f"已提取 {count} 帧到 {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
