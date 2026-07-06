"""流式取消验证脚本。

验证 cancel() 后 Pipeline 不再处理新帧，且内存被释放。

用法：
    uv run --extra vision python scripts/test_cancel.py
"""

from __future__ import annotations

import time
from pathlib import Path

from sublift.config import ChangePointConfig, Config
from sublift.detector.fixed_region import FixedRegionDetector
from sublift.extractor.ffmpeg_extractor import FfmpegExtractor
from sublift.models import BoundingBox
from sublift.ocr.vision import VisionOcrEngine
from sublift.pipeline.core import Pipeline

VIDEO_PATH = Path("debug/Zootopia_clip_1080p.mp4")
REGION = BoundingBox(x=0, y=860, width=1920, height=220)
FPS = 5.0


def main() -> None:
    if not VIDEO_PATH.exists():
        print(f"视频不存在: {VIDEO_PATH}")
        return

    config = Config(
        sample_fps=FPS,
        confidence_threshold=0.5,
        change_point=ChangePointConfig(enable_ssim_patrol=True),
    )

    extractor = FfmpegExtractor(fps=FPS)
    pipeline = Pipeline(
        detector=FixedRegionDetector(REGION),
        ocr=VisionOcrEngine(),
        config=config,
    )
    frames = extractor.extract(VIDEO_PATH)

    # 处理前 50 帧后取消
    frame_count = 0
    entries_before_cancel = 0
    t0 = time.time()

    for frame_count, frame in enumerate(frames, 1):
        if frame_count > 50:
            pipeline.cancel()
            print(f"在第 {frame_count} 帧后取消")
            break
        event = pipeline.feed(frame)
        if event is not None:
            entry = pipeline.ocr_segment(event)
            entries_before_cancel += 1
            print(f"  增量 #{entries_before_cancel}: {entry.text[:20]} @ {entry.start_ms}ms")

    # 取消后再喂帧（应该被忽略）
    remaining = 0
    for frame in frames:
        event = pipeline.feed(frame)
        if event is not None:
            remaining += 1

    t_elapsed = time.time() - t0
    print(f"\n取消前增量: {entries_before_cancel} 条")
    print(f"取消后增量: {remaining} 条（应为 0）")
    print(f"已处理帧数: {pipeline.processed_count}（取消后应归零）")
    print(f"耗时: {t_elapsed:.2f}s")


if __name__ == "__main__":
    main()
