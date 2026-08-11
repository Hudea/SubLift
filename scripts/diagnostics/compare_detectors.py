"""将指定视频的前几帧按两种字幕检测器裁剪，供人工比较。"""

from __future__ import annotations

import argparse
from pathlib import Path

from sublift.detector import BottomCropDetector, FixedRegionDetector
from sublift.extractor import FfmpegExtractor
from sublift.models import BoundingBox


def _parse_region(raw: str) -> BoundingBox:
    try:
        x, y, width, height = (int(value) for value in raw.split(","))
    except ValueError as exc:
        raise argparse.ArgumentTypeError("区域必须是 x,y,width,height") from exc
    if width <= 0 or height <= 0:
        raise argparse.ArgumentTypeError("区域宽高必须为正")
    return BoundingBox(x=x, y=y, width=width, height=height)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="比较 BottomCrop 与 FixedRegion 的裁剪结果")
    parser.add_argument("--video", type=Path, required=True, help="本地视频路径")
    parser.add_argument("--out", type=Path, required=True, help="输出目录")
    parser.add_argument("--fps", type=float, default=1.0)
    parser.add_argument("--limit", type=int, default=10, help="每种检测器最多输出的帧数")
    parser.add_argument("--bottom-ratio", type=float, default=0.3)
    parser.add_argument(
        "--fixed-region",
        type=_parse_region,
        default=_parse_region("200,1500,3440,400"),
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.fps <= 0 or args.limit <= 0 or not 0 < args.bottom_ratio <= 1:
        print("--fps、--limit 与 --bottom-ratio 必须在有效范围内")
        return 2
    video = args.video.expanduser().resolve()
    if not video.is_file():
        print(f"视频不存在: {video}")
        return 2

    out_root = args.out.expanduser().resolve()
    detectors: dict[str, BottomCropDetector | FixedRegionDetector] = {
        "bottom_crop": BottomCropDetector(bottom_ratio=args.bottom_ratio),
        "fixed_region": FixedRegionDetector(region=args.fixed_region),
    }
    extractor = FfmpegExtractor(fps=args.fps)
    frames = []
    for i, frame in enumerate(extractor.extract(video)):
        if i >= args.limit:
            break
        frames.append(frame)

    for name, detector in detectors.items():
        out_dir = out_root / name
        out_dir.mkdir(parents=True, exist_ok=True)
        for frame in frames:
            region = detector.detect(frame)
            b = region.box
            cropped = frame.image.crop((b.x, b.y, b.x + b.width, b.y + b.height))
            path = out_dir / f"frame_{frame.timestamp_ms:06d}.png"
            cropped.save(path)
        print(f"  {name}/  → {len(frames)} 帧")

    print(f"总计: {len(frames)} 帧 × {len(detectors)} 检测器 = {len(frames) * len(detectors)} 张图")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
