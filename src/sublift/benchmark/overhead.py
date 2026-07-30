"""Measure summary-mode overhead against performance mode off."""

from __future__ import annotations

import hashlib
import json
from dataclasses import replace
from pathlib import Path
from typing import Any

from sublift.benchmark.config import RunConfig
from sublift.benchmark.runner import RunResult, _execute_run, _probe_duration
from sublift.diagnostics.performance import PerformanceMode


def run_overhead(
    config: RunConfig,
    *,
    runs: int = 3,
    target_pct: float = 5.0,
) -> tuple[dict[str, Any], Path]:
    """Run interleaved off/summary pairs and write ``overhead.json``."""
    if runs < 1:
        raise ValueError("runs 必须 >= 1")
    if target_pct < 0:
        raise ValueError("target_pct 不能为负")
    if not config.video_path.exists():
        raise FileNotFoundError(f"视频文件不存在: {config.video_path}")

    duration = config.video_duration_seconds or _probe_duration(config.video_path)
    off_config = replace(
        config,
        performance_mode=PerformanceMode.OFF.value,
        warmup_runs=0,
        measured_runs=1,
    )
    summary_config = replace(off_config, performance_mode=PerformanceMode.SUMMARY.value)

    off_times: list[float] = []
    summary_times: list[float] = []
    off_hashes: list[str] = []
    summary_hashes: list[str] = []
    for run_index in range(1, runs + 1):
        off_result, _off_perf = _execute_run(
            off_config,
            mode=PerformanceMode.OFF,
            video_duration=duration,
            isolate=config.isolate_processes,
            segment_path=None,
        )
        off_times.append(off_result.elapsed_seconds)
        off_hashes.append(_detection_hash(off_result))

        summary_result, summary_perf = _execute_run(
            summary_config,
            mode=PerformanceMode.SUMMARY,
            video_duration=duration,
            isolate=config.isolate_processes,
            segment_path=None,
        )
        summary_times.append(_summary_wall(summary_result, summary_perf))
        summary_hashes.append(_detection_hash(summary_result))
        print(
            f"pair {run_index}/{runs}: "
            f"off={off_times[-1]:.3f}s summary={summary_times[-1]:.3f}s"
        )

    off_median = _median(off_times)
    summary_median = _median(summary_times)
    overhead_pct = (
        (summary_median - off_median) / off_median * 100.0 if off_median > 0 else 0.0
    )
    hashes = off_hashes + summary_hashes
    detections_consistent = len(set(hashes)) == 1
    report: dict[str, Any] = {
        "schema_version": 2,
        "protocol": "interleaved_off_summary_pairs",
        "runs_per_mode": runs,
        "off_seconds": off_times,
        "summary_seconds": summary_times,
        "off_median": off_median,
        "summary_median": summary_median,
        "overhead_pct": overhead_pct,
        "target_pct": target_pct,
        "detection_hashes": {
            "off": off_hashes,
            "summary": summary_hashes,
            "consistent": detections_consistent,
        },
        "pass": overhead_pct <= target_pct and detections_consistent,
    }
    config.output_dir.mkdir(parents=True, exist_ok=True)
    path = config.output_dir / "overhead.json"
    path.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    return report, path


def _summary_wall(result: RunResult, performance: dict[str, Any] | None) -> float:
    throughput = (performance or {}).get("throughput")
    if isinstance(throughput, dict):
        core_wall_ms = throughput.get("core_wall_ms")
        if isinstance(core_wall_ms, int | float) and not isinstance(core_wall_ms, bool):
            return float(core_wall_ms) / 1000.0
    return result.elapsed_seconds


def _detection_hash(result: RunResult) -> str:
    payload = [
        [entry.start_ms, entry.end_ms, entry.text]
        for entry in result.detected
    ]
    raw = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode()
    return hashlib.sha256(raw).hexdigest()[:16]


def _median(values: list[float]) -> float:
    ordered = sorted(values)
    midpoint = len(ordered) // 2
    if len(ordered) % 2:
        return ordered[midpoint]
    return (ordered[midpoint - 1] + ordered[midpoint]) / 2.0
