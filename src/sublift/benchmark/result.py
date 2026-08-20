"""Benchmark run result model.

Kept separate from the extract runner so scoring/report imports stay free of
Pipeline, OCR, OpenCV and Pillow.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any

from sublift.benchmark.config import RunConfig
from sublift.benchmark.srt import SrtEntry


@dataclass(frozen=True)
class RunResult:
    """一次 benchmark 运行的完整结果。"""

    config: RunConfig
    detected: list[SrtEntry]
    ground_truth: list[SrtEntry]
    elapsed_seconds: float
    video_duration_seconds: float
    exported_srt_path: Path | None = None
    performance: dict[str, Any] | None = None
