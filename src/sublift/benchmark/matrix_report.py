"""Aggregate reports for multi-parameter benchmark matrices."""

from __future__ import annotations

import csv
import io
import json
from collections.abc import Mapping
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

from sublift.benchmark.diagnostics import analyze_result
from sublift.benchmark.result import RunResult


@dataclass(frozen=True)
class MatrixRecord:
    """One completed or failed matrix cell."""

    label: str
    overrides: dict[str, Any]
    status: str
    metrics: dict[str, float | int | None]
    artifacts: dict[str, str]
    error: str | None = None


def completed_record(
    *,
    label: str,
    overrides: Mapping[str, Any],
    result: RunResult,
    artifacts: Mapping[str, Path],
) -> MatrixRecord:
    """Build one aggregate row from a completed benchmark."""
    analysis = analyze_result(result)
    timing = analysis.metrics.timing
    recognition = analysis.metrics.recognition
    e2e = analysis.metrics.e2e
    speed_factor = (
        result.video_duration_seconds / result.elapsed_seconds
        if result.video_duration_seconds > 0 and result.elapsed_seconds > 0
        else 0.0
    )
    status = str(analysis.summary["status"])
    return MatrixRecord(
        label=label,
        overrides=dict(overrides),
        status=status,
        metrics={
            "detected": len(result.detected),
            "timing_recall": timing.timing_recall,
            "timing_precision": timing.timing_precision,
            "timing_f1": timing.timing_f1,
            "boundary_p95_ms": timing.boundary_p95_ms,
            "cer_macro": recognition.cer_macro,
            "cer_micro": recognition.cer_micro,
            "exact_match_rate": recognition.exact_match_rate,
            "usable_subtitle_recall": e2e.usable_subtitle_recall,
            "elapsed_seconds": result.elapsed_seconds,
            "speed_factor": speed_factor,
        },
        artifacts={key: str(path) for key, path in artifacts.items()},
    )


def failed_record(
    *,
    label: str,
    overrides: Mapping[str, Any],
    error: str,
) -> MatrixRecord:
    """Build one aggregate row for a failed matrix cell."""
    return MatrixRecord(
        label=label,
        overrides=dict(overrides),
        status="error",
        metrics={},
        artifacts={},
        error=error,
    )


def write_matrix_reports(records: list[MatrixRecord], output_dir: Path) -> dict[str, Path]:
    """Write matrix JSON, CSV and Markdown summaries."""
    output_dir.mkdir(parents=True, exist_ok=True)
    paths = {
        "matrix_json": output_dir / "matrix.results.json",
        "matrix_csv": output_dir / "matrix.results.csv",
        "matrix_markdown": output_dir / "matrix.summary.md",
    }
    payload = {
        "schema_version": 1,
        "case_count": len(records),
        "passed": sum(record.status == "pass" for record in records),
        "failed": sum(record.status != "pass" for record in records),
        "records": [asdict(record) for record in records],
    }
    paths["matrix_json"].write_text(
        json.dumps(payload, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    paths["matrix_csv"].write_text(_format_csv(records), encoding="utf-8")
    paths["matrix_markdown"].write_text(_format_markdown(records), encoding="utf-8")
    return paths


def _format_csv(records: list[MatrixRecord]) -> str:
    buffer = io.StringIO()
    fields = [
        "label",
        "status",
        "overrides",
        "detected",
        "timing_f1",
        "timing_precision",
        "boundary_p95_ms",
        "cer_macro",
        "exact_match_rate",
        "usable_subtitle_recall",
        "elapsed_seconds",
        "speed_factor",
        "error",
    ]
    writer = csv.DictWriter(buffer, fieldnames=fields, lineterminator="\n")
    writer.writeheader()
    for record in records:
        writer.writerow(
            {
                "label": record.label,
                "status": record.status,
                "overrides": json.dumps(record.overrides, ensure_ascii=False, sort_keys=True),
                **{field: record.metrics.get(field) for field in fields if field in record.metrics},
                "error": record.error or "",
            }
        )
    return buffer.getvalue()


def _format_markdown(records: list[MatrixRecord]) -> str:
    lines = [
        "# Benchmark Matrix Summary",
        "",
        f"- cases: {len(records)}",
        f"- passed: {sum(record.status == 'pass' for record in records)}",
        f"- failed: {sum(record.status != 'pass' for record in records)}",
        "",
        "| Label | Status | F1 | CER macro | Exact | Usable | Wall | Speed |",
        "|---|---|---:|---:|---:|---:|---:|---:|",
    ]
    for record in records:
        metrics = record.metrics
        lines.append(
            "| {label} | {status} | {f1} | {cer} | {exact} | {usable} | {wall} | {speed} |".format(
                label=record.label,
                status=record.status,
                f1=_pct(metrics.get("timing_f1")),
                cer=_pct(metrics.get("cer_macro")),
                exact=_pct(metrics.get("exact_match_rate")),
                usable=_pct(metrics.get("usable_subtitle_recall")),
                wall=_number(metrics.get("elapsed_seconds"), "s"),
                speed=_number(metrics.get("speed_factor"), "×"),
            )
        )
    lines.append("")
    errors = [record for record in records if record.error]
    if errors:
        lines.extend(["## Errors", ""])
        for record in errors:
            lines.append(f"- `{record.label}`: {record.error}")
        lines.append("")
    return "\n".join(lines)


def _pct(value: float | int | None) -> str:
    if value is None:
        return "—"
    return f"{float(value) * 100:.2f}%"


def _number(value: float | int | None, suffix: str) -> str:
    if value is None:
        return "—"
    return f"{float(value):.2f}{suffix}"
