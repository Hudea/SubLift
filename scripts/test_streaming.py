"""真实视频流式 Pipeline 验证脚本。

对比流式 API（feed/ocr_segment/finalize）与批量 API（run_frames）的结果一致性，
并测量首条字幕反馈时间。

用法：
    uv run python scripts/test_streaming.py
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

    # 1. 批量模式（基线）
    print("=== 批量模式 ===")
    extractor_batch = FfmpegExtractor(fps=FPS)
    pipeline_batch = Pipeline(
        detector=FixedRegionDetector(REGION),
        ocr=VisionOcrEngine(),
        config=config,
        extractor=extractor_batch,
    )
    t0 = time.time()
    batch_result = pipeline_batch.run(VIDEO_PATH)
    t_batch = time.time() - t0
    print(f"批量：{len(batch_result)} 条，耗时 {t_batch:.2f}s")

    # 2. 流式模式
    print("\n=== 流式模式 ===")
    extractor_stream = FfmpegExtractor(fps=FPS)
    pipeline_stream = Pipeline(
        detector=FixedRegionDetector(REGION),
        ocr=VisionOcrEngine(),
        config=config,
    )
    frames = extractor_stream.extract(VIDEO_PATH)

    incremental_entries = []
    t0 = time.time()
    first_entry_time = None

    for _frame_count, frame in enumerate(frames, 1):
        event = pipeline_stream.feed(frame)
        if event is not None:
            entry = pipeline_stream.ocr_segment(event)
            incremental_entries.append(entry)
            if first_entry_time is None:
                first_entry_time = time.time() - t0
                print(f"首条字幕反馈时间: {first_entry_time:.2f}s")
                print(f"  文本: {entry.text[:30]}")
                print(f"  时间: {entry.start_ms}ms - {entry.end_ms}ms")

    final_result = pipeline_stream.finalize()
    t_stream = time.time() - t0
    print(
        f"流式：{len(incremental_entries)} 条增量，"
        f"{len(final_result)} 条最终，耗时 {t_stream:.2f}s"
    )

    # 3. 对比
    print("\n=== 对比 ===")
    print(f"批量结果: {len(batch_result)} 条")
    print(f"流式最终: {len(final_result)} 条")
    match = batch_result == final_result
    print(f"结果一致: {match}")

    if not match:
        print("\n差异明细：")
        max_len = max(len(batch_result), len(final_result))
        for i in range(max_len):
            b = batch_result[i] if i < len(batch_result) else None
            s = final_result[i] if i < len(final_result) else None
            if b != s:
                print(f"  [{i}] 批量: {b}")
                print(f"       流式: {s}")

    # 4. 内存验证
    print("\n=== 内存验证 ===")
    print(f"已处理帧数: {pipeline_stream.processed_count}")
    print(f"增量 entries: {len(incremental_entries)}")
    print(f"最终 entries: {len(final_result)}")


if __name__ == "__main__":
    main()
