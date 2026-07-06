"""Benchmark diagnostic report output.

Reports are generated from one diagnostic model instead of maintaining a
separate legacy metric/report path.
"""

from __future__ import annotations

from pathlib import Path

from benchmark.diagnostics import (
    analyze_result,
    format_agent_json,
    format_det_cases_csv,
    format_gt_cases_csv,
    format_summary_markdown,
)
from benchmark.runner import RunResult


def write_reports(
    result: RunResult,
    *,
    baseline_timing_precision: float | None = None,
) -> dict[str, Path]:
    """Write agent-readable benchmark diagnostics.

    Outputs:
        ``.agent.json``: structured summary, gates, metrics, clusters and cases.
        ``.gt_cases.csv``: one row per ground-truth subtitle.
        ``.det_cases.csv``: one row per detected subtitle segment.
        ``.summary.md``: compact human-readable summary.
    """
    output_dir = result.config.output_dir
    output_dir.mkdir(parents=True, exist_ok=True)
    prefix = result.config.output_prefix

    paths = {
        "agent_json": output_dir / f"{prefix}.agent.json",
        "gt_cases_csv": output_dir / f"{prefix}.gt_cases.csv",
        "det_cases_csv": output_dir / f"{prefix}.det_cases.csv",
        "summary_markdown": output_dir / f"{prefix}.summary.md",
    }
    analysis = analyze_result(
        result,
        baseline_timing_precision=baseline_timing_precision,
    )

    paths["agent_json"].write_text(
        format_agent_json(result, analysis, artifacts=paths),
        encoding="utf-8",
    )
    paths["gt_cases_csv"].write_text(format_gt_cases_csv(analysis), encoding="utf-8")
    paths["det_cases_csv"].write_text(format_det_cases_csv(analysis), encoding="utf-8")
    paths["summary_markdown"].write_text(
        format_summary_markdown(result, analysis, artifacts=paths),
        encoding="utf-8",
    )

    return paths
