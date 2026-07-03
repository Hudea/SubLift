"""用两种检测器裁剪前 10 帧，对比字幕区域效果。"""

from pathlib import Path

from sublift.detector import BottomCropDetector, FixedRegionDetector
from sublift.extractor import FfmpegExtractor
from sublift.models import BoundingBox

video = Path("debug/Zootopia_clip_hardsub1.mkv")
out_root = Path("debug/detector_test")

detectors: dict[str, BottomCropDetector | FixedRegionDetector] = {
    "bottom_crop_30pct": BottomCropDetector(bottom_ratio=0.3),
    "fixed_region_center": FixedRegionDetector(
        region=BoundingBox(x=200, y=1500, width=3440, height=400),
    ),
}

extractor = FfmpegExtractor(fps=1.0)
frames = []
for i, frame in enumerate(extractor.extract(video)):
    if i >= 10:
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
