"""Score an existing SRT without running extraction."""

from __future__ import annotations

from pathlib import Path

from sublift.benchmark.config import RunConfig
from sublift.benchmark.media import probe_duration_seconds
from sublift.benchmark.result import RunResult
from sublift.benchmark.srt import load_srt


def align_existing_srt(
    config: RunConfig,
    detected_path: Path,
    *,
    elapsed_seconds: float | None = None,
    video_duration_seconds: float | None = None,
) -> RunResult:
    """对已存在的 SRT 产物计算指标（不跑提取）。"""
    if not detected_path.exists():
        raise FileNotFoundError(f"检测产物 SRT 不存在: {detected_path}")
    detected = load_srt(detected_path)
    ground_truth = load_srt(config.ground_truth_path)
    duration = video_duration_seconds
    if duration is None:
        duration = config.video_duration_seconds
    if duration is None and config.video_path.exists():
        duration = probe_duration_seconds(config.video_path)
    if duration is None:
        duration = 0.0
    return RunResult(
        config=config,
        detected=detected,
        ground_truth=ground_truth,
        elapsed_seconds=float(elapsed_seconds or 0.0),
        video_duration_seconds=float(duration),
        exported_srt_path=detected_path,
    )
