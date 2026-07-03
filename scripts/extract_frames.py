"""从测试视频抽取前 30 帧（前 30 秒，1 fps）到 debug/extractor_test/。"""

from pathlib import Path

from sublift.extractor import FfmpegExtractor

video = Path("debug/Zootopia_clip_hardsub1.mkv")
out_dir = Path("debug/extractor_test")
out_dir.mkdir(parents=True, exist_ok=True)

extractor = FfmpegExtractor(fps=1.0)
count = 0
for frame in extractor.extract(video):
    if count >= 30:
        break
    path = out_dir / f"frame_{frame.timestamp_ms:06d}.png"
    frame.image.save(path)
    count += 1

print(f"已提取 {count} 帧到 {out_dir}")
