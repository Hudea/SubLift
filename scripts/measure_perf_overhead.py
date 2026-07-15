"""Measure summary-mode overhead vs off on the canonical fixed-region load.

Usage:
    uv run python scripts/measure_perf_overhead.py
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parents[1]
if str(_REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(_REPO_ROOT))

from benchmark.runner import (  # noqa: E402
    PerformanceMode,
    RunConfig,
    _execute_run,
    _probe_duration,
)


def _median(values: list[float]) -> float:
    ordered = sorted(values)
    mid = len(ordered) // 2
    if len(ordered) % 2:
        return ordered[mid]
    return (ordered[mid - 1] + ordered[mid]) / 2.0


def main() -> int:
    video = _REPO_ROOT / "debug/Zootopia_clip_1080p.mp4"
    gt = _REPO_ROOT / "benchmark/fixtures/Zootopia_clip_1080p_gt.srt"
    out = _REPO_ROOT / "debug/perf_reports/feat037_overhead"
    out.mkdir(parents=True, exist_ok=True)

    if not video.exists():
        print(f"missing video: {video}", file=sys.stderr)
        return 1

    dur = _probe_duration(video)
    base = RunConfig(
        video_path=video,
        ground_truth_path=gt,
        fps=5.0,
        engine="vision",
        confidence=0.5,
        subtitle_script="cjk",
        region_box=(0, 848, 1920, 87),
        label="overhead",
        output_dir=out,
        performance_mode="off",
        warmup_runs=0,
        measured_runs=1,
        isolate_processes=True,
    )

    off_times: list[float] = []
    for i in range(3):
        result, _perf = _execute_run(
            base,
            mode=PerformanceMode.OFF,
            video_duration=dur,
            isolate=True,
            segment_path=None,
        )
        off_times.append(result.elapsed_seconds)
        print(f"off run{i + 1}: {result.elapsed_seconds:.3f}s entries={len(result.detected)}")

    from dataclasses import replace

    summary_cfg = replace(base, performance_mode="summary")
    sum_times: list[float] = []
    for i in range(3):
        result, perf = _execute_run(
            summary_cfg,
            mode=PerformanceMode.SUMMARY,
            video_duration=dur,
            isolate=True,
            segment_path=None,
        )
        core_ms = (perf or {}).get("throughput", {}).get("core_wall_ms")
        if isinstance(core_ms, int | float):
            wall = float(core_ms) / 1000.0
        else:
            wall = result.elapsed_seconds
        sum_times.append(wall)
        print(
            f"summary run{i + 1}: wall={wall:.3f}s elapsed={result.elapsed_seconds:.3f}s "
            f"entries={len(result.detected)}"
        )

    off_med = _median(off_times)
    sum_med = _median(sum_times)
    overhead = (sum_med - off_med) / off_med * 100.0 if off_med else 0.0
    report = {
        "off_seconds": off_times,
        "summary_seconds": sum_times,
        "off_median": off_med,
        "summary_median": sum_med,
        "overhead_pct": overhead,
        "target_pct": 5.0,
        "pass": overhead <= 5.0,
    }
    path = out / "overhead.json"
    path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print("---")
    print(f"off median={off_med:.3f}s  summary median={sum_med:.3f}s  overhead={overhead:.2f}%")
    print(f"wrote {path}")
    return 0 if report["pass"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
