"""Performance-recorder overhead protocol tests."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import pytest

import sublift.benchmark.overhead as overhead
from sublift.benchmark.config import RunConfig
from sublift.benchmark.result import RunResult
from sublift.benchmark.srt import SrtEntry
from sublift.diagnostics.performance import PerformanceMode


def test_run_overhead_interleaves_modes_and_checks_detection_hash(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    video = tmp_path / "movie.mp4"
    video.write_bytes(b"fixture")
    config = RunConfig(
        video_path=video,
        ground_truth_path=tmp_path / "gt.srt",
        output_dir=tmp_path / "report",
        video_duration_seconds=10.0,
        isolate_processes=False,
    )
    modes: list[PerformanceMode] = []
    entry = SrtEntry(index=1, start_ms=1000, end_ms=2000, text="hello")

    def fake_execute(
        run_config: RunConfig,
        *,
        mode: PerformanceMode,
        video_duration: float,
        isolate: bool,
        segment_path: Path | None,
    ) -> tuple[RunResult, dict[str, Any] | None]:
        del isolate, segment_path
        modes.append(mode)
        elapsed = 1.0 if mode is PerformanceMode.OFF else 1.02
        performance = (
            None
            if mode is PerformanceMode.OFF
            else {"throughput": {"core_wall_ms": elapsed * 1000.0}}
        )
        return (
            RunResult(
                config=run_config,
                detected=[entry],
                ground_truth=[entry],
                elapsed_seconds=elapsed,
                video_duration_seconds=video_duration,
            ),
            performance,
        )

    monkeypatch.setattr(overhead, "_execute_run", fake_execute)

    report, report_path = overhead.run_overhead(config, runs=2, target_pct=5.0)

    assert modes == [
        PerformanceMode.OFF,
        PerformanceMode.SUMMARY,
        PerformanceMode.OFF,
        PerformanceMode.SUMMARY,
    ]
    assert report["overhead_pct"] == pytest.approx(2.0)
    assert report["detection_hashes"]["consistent"] is True
    assert report["pass"] is True
    assert json.loads(report_path.read_text(encoding="utf-8"))["protocol"] == (
        "interleaved_off_summary_pairs"
    )
