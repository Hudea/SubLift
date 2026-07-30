#!/usr/bin/env python3
"""Run the Phase 6.8 multi-source Paddle E2E quality gate.

Unlike the retired placeholder implementation, every reported metric comes
from a real ``sublift extract --engine paddle --runtime python|cpp`` product
run, the shared benchmark diagnostics, or a live Q0 box replay. Missing
assets, models, workers, baselines, and runtime failures are blocking errors.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
import tempfile
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any, cast

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from scripts.parity.dump_paddle_stages import build_live_report  # noqa: E402
from scripts.parity.gen_paddle_quality_assets import (  # noqa: E402
    generate_assets,
)

from sublift.benchmark.diagnostics import (  # noqa: E402
    TEXT_EMPTY,
    TEXT_NOISE,
    BenchmarkDiagnostics,
    analyze_entries,
)
from sublift.benchmark.srt import SrtEntry, load_srt  # noqa: E402
from sublift.models import BoundingBox  # noqa: E402
from sublift.runtime import probe_cpp_paddle_available  # noqa: E402

DEFAULT_MANIFEST = (
    REPO_ROOT
    / "benchmark"
    / "fixtures"
    / "paddle_quality"
    / "manifest.v1.json"
)
DEFAULT_BASELINE = (
    REPO_ROOT
    / "benchmark"
    / "parity"
    / "goldens"
    / "paddle"
    / "paddle_quality_baseline.v1.json"
)
FREEZE_MANIFEST = REPO_ROOT / "scripts" / "parity" / "freeze_paddle_manifest.json"


@dataclass
class PaddleGateMetrics:
    timing_f1: float = 0.0
    timing_precision: float = 0.0
    timing_recall: float = 0.0
    cer_macro: float = 0.0
    cer_micro: float = 0.0
    usable_recall: float = 0.0
    noise_count: int = 0
    empty_count: int = 0
    line_box_mean_iou: float = 1.0
    line_box_recall_at_05: float = 1.0


@dataclass
class GateEvaluationResult:
    passed: bool
    name: str
    oracle_val: float
    candidate_val: float
    delta: float
    allowed_limit: float
    message: str


@dataclass
class PaddleGateReport:
    overall_passed: bool
    metrics_oracle: PaddleGateMetrics
    metrics_candidate: PaddleGateMetrics
    gate_results: list[GateEvaluationResult] = field(default_factory=list)
    absolute_gate_results: list[GateEvaluationResult] = field(
        default_factory=list
    )
    source_results: list[dict[str, Any]] = field(default_factory=list)
    fingerprint: dict[str, Any] = field(default_factory=dict)


def compute_box_iou(box1: BoundingBox, box2: BoundingBox) -> float:
    """Compute axis-aligned intersection over union."""
    x_left = max(box1.x, box2.x)
    y_top = max(box1.y, box2.y)
    x_right = min(box1.x + box1.width, box2.x + box2.width)
    y_bottom = min(box1.y + box1.height, box2.y + box2.height)
    if x_right <= x_left or y_bottom <= y_top:
        return 0.0
    intersection = (x_right - x_left) * (y_bottom - y_top)
    union = (
        box1.width * box1.height
        + box2.width * box2.height
        - intersection
    )
    return float(intersection) / float(union) if union > 0 else 0.0


def _gate(
    *,
    name: str,
    oracle: float,
    candidate: float,
    passed: bool,
    limit: float,
    failure: str,
) -> GateEvaluationResult:
    return GateEvaluationResult(
        passed=passed,
        name=name,
        oracle_val=oracle,
        candidate_val=candidate,
        delta=candidate - oracle,
        allowed_limit=limit,
        message="PASS" if passed else f"FAIL ({failure})",
    )


def evaluate_paddle_hard_gates(
    oracle: PaddleGateMetrics,
    candidate: PaddleGateMetrics,
) -> PaddleGateReport:
    """Evaluate all Candidate-relative 06805 hard gates."""
    gates = [
        _gate(
            name="timing_f1_relative",
            oracle=oracle.timing_f1,
            candidate=candidate.timing_f1,
            passed=candidate.timing_f1 >= oracle.timing_f1 - 0.010,
            limit=-0.010,
            failure="timing F1 drop exceeds 1pp",
        ),
        _gate(
            name="timing_precision_relative",
            oracle=oracle.timing_precision,
            candidate=candidate.timing_precision,
            passed=(
                candidate.timing_precision
                >= oracle.timing_precision - 0.010
            ),
            limit=-0.010,
            failure="timing precision drop exceeds 1pp",
        ),
        _gate(
            name="cer_macro_relative",
            oracle=oracle.cer_macro,
            candidate=candidate.cer_macro,
            passed=candidate.cer_macro <= oracle.cer_macro + 0.010,
            limit=0.010,
            failure="CER macro increase exceeds 1pp",
        ),
        _gate(
            name="cer_micro_relative",
            oracle=oracle.cer_micro,
            candidate=candidate.cer_micro,
            passed=candidate.cer_micro <= oracle.cer_micro + 0.010,
            limit=0.010,
            failure="CER micro increase exceeds 1pp",
        ),
        _gate(
            name="usable_recall_relative",
            oracle=oracle.usable_recall,
            candidate=candidate.usable_recall,
            passed=candidate.usable_recall >= oracle.usable_recall - 0.020,
            limit=-0.020,
            failure="usable recall drop exceeds 2pp",
        ),
        _gate(
            name="noise_count_no_systematic_increase",
            oracle=float(oracle.noise_count),
            candidate=float(candidate.noise_count),
            passed=candidate.noise_count <= oracle.noise_count,
            limit=0.0,
            failure="aggregate noise count increased",
        ),
        _gate(
            name="empty_count_no_systematic_increase",
            oracle=float(oracle.empty_count),
            candidate=float(candidate.empty_count),
            passed=candidate.empty_count <= oracle.empty_count,
            limit=0.0,
            failure="aggregate empty-text count increased",
        ),
        _gate(
            name="line_box_recall_at_05",
            oracle=oracle.line_box_recall_at_05,
            candidate=candidate.line_box_recall_at_05,
            passed=candidate.line_box_recall_at_05 >= 0.98,
            limit=0.98,
            failure="box recall at IoU 0.5 is below 0.98",
        ),
        _gate(
            name="line_box_mean_iou",
            oracle=oracle.line_box_mean_iou,
            candidate=candidate.line_box_mean_iou,
            passed=candidate.line_box_mean_iou >= 0.90,
            limit=0.90,
            failure="mean box IoU is below 0.90",
        ),
    ]
    return PaddleGateReport(
        overall_passed=all(item.passed for item in gates),
        metrics_oracle=oracle,
        metrics_candidate=candidate,
        gate_results=gates,
    )


def evaluate_source_output_hashes(
    source_results: list[dict[str, Any]],
) -> GateEvaluationResult:
    """Require byte-identical product SRT for every frozen source."""
    matched = sum(
        1
        for source in source_results
        if source["oracle_output_sha256"] == source["candidate_output_sha256"]
    )
    total = len(source_results)
    return _gate(
        name="source_output_sha256_exact",
        oracle=float(total),
        candidate=float(matched),
        passed=matched == total,
        limit=float(total),
        failure=f"only {matched}/{total} source outputs are byte-identical",
    )


def _sha256(path: Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def _git_head() -> str:
    result = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=True,
    )
    return result.stdout.strip()


def _resolve_path(value: str) -> Path:
    path = Path(value)
    return path if path.is_absolute() else REPO_ROOT / path


def _load_and_validate_manifest(path: Path) -> dict[str, Any]:
    manifest = cast(
        dict[str, Any],
        json.loads(path.read_text(encoding="utf-8")),
    )
    sources = manifest.get("sources")
    if not isinstance(sources, list):
        raise RuntimeError("Paddle quality manifest sources must be a list")
    minimum_sources = int(manifest["minimum_source_count"])
    if len(sources) < minimum_sources:
        raise RuntimeError(
            f"Paddle Q2 needs at least {minimum_sources} sources"
        )
    total_duration = sum(float(item["duration_seconds"]) for item in sources)
    minimum_duration = float(manifest["minimum_total_duration_seconds"])
    if total_duration < minimum_duration:
        raise RuntimeError(
            f"Paddle Q2 duration {total_duration:.3f}s < {minimum_duration:.3f}s"
        )
    tags = {str(tag) for item in sources for tag in item.get("tags", [])}
    required_tags = {"cjk", "latin", "mixed", "blank-intervals"}
    if not required_tags.issubset(tags):
        raise RuntimeError(
            "Paddle Q2 style coverage is incomplete: "
            f"{sorted(required_tags.difference(tags))}"
        )
    q1_scripts = {
        str(item["script"])
        for item in sources
        if float(item.get("q1_window_seconds", 0.0)) >= 30.0
    }
    if not {"cjk", "latin", "auto"}.issubset(q1_scripts):
        raise RuntimeError(
            "Paddle Q1 must freeze >=30s windows for CJK, Latin, and mixed"
        )

    if any(str(item.get("kind")) == "generated" for item in sources):
        generate_assets()
    for source in sources:
        for path_key, hash_key in (
            ("video", "video_sha256"),
            ("ground_truth", "ground_truth_sha256"),
            ("subtitle_recipe", "subtitle_recipe_sha256"),
        ):
            if path_key not in source:
                continue
            asset = _resolve_path(str(source[path_key]))
            if not asset.is_file():
                raise RuntimeError(
                    f"missing Paddle Q2 asset for {source['id']}: {asset}"
                )
            expected = str(source[hash_key])
            actual = _sha256(asset)
            if actual != expected:
                raise RuntimeError(
                    f"Paddle Q2 hash mismatch for {asset}: {actual} != {expected}"
                )

    generator = manifest.get("generator")
    if isinstance(generator, dict):
        script = _resolve_path(str(generator["script"]))
        if _sha256(script) != str(generator["script_sha256"]):
            raise RuntimeError("Paddle Q2 generator script hash mismatch")
        for font in generator.get("fonts", []):
            font_path = Path(str(font["path"]))
            if not font_path.is_file():
                raise RuntimeError(f"Paddle Q2 frozen font is missing: {font_path}")
            if _sha256(font_path) != str(font["sha256"]):
                raise RuntimeError(
                    f"Paddle Q2 frozen font hash mismatch: {font_path}"
                )
    return manifest


@dataclass(frozen=True)
class _ProductRun:
    runtime: str
    elapsed_seconds: float
    entries: list[SrtEntry]
    output_sha256: str
    stdout_tail: str
    stderr_tail: str


def _run_product(
    source: dict[str, Any],
    *,
    runtime: str,
    output_path: Path,
) -> _ProductRun:
    video = _resolve_path(str(source["video"]))
    print(
        f"[Paddle Quality Gate] {source['id']}: "
        f"runtime={runtime} product run...",
        flush=True,
    )
    command = [
        sys.executable,
        "-m",
        "sublift.cli",
        "extract",
        str(video),
        "-o",
        str(output_path),
        "--engine",
        "paddle",
        "--runtime",
        runtime,
        "--fps",
        str(source["fps"]),
        "--confidence",
        "0.5",
        "--script",
        str(source["script"]),
    ]
    start = time.perf_counter()
    completed = subprocess.run(
        command,
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        timeout=max(180.0, float(source["duration_seconds"]) * 5.0),
        check=False,
    )
    elapsed = time.perf_counter() - start
    if completed.returncode != 0:
        detail = (completed.stderr or completed.stdout).strip()
        raise RuntimeError(
            f"Paddle {runtime} product run failed for {source['id']}: {detail}"
        )
    if not output_path.is_file():
        raise RuntimeError(
            f"Paddle {runtime} product run did not create {output_path}"
        )
    result = _ProductRun(
        runtime=runtime,
        elapsed_seconds=elapsed,
        entries=load_srt(output_path),
        output_sha256=_sha256(output_path),
        stdout_tail="\n".join(completed.stdout.splitlines()[-6:]),
        stderr_tail="\n".join(completed.stderr.splitlines()[-6:]),
    )
    print(
        f"[Paddle Quality Gate] {source['id']}: runtime={runtime} "
        f"wall={result.elapsed_seconds:.3f}s entries={len(result.entries)}",
        flush=True,
    )
    return result


def _metrics_from_analysis(
    analysis: BenchmarkDiagnostics,
    *,
    box_mean_iou: float = 1.0,
    box_recall: float = 1.0,
) -> PaddleGateMetrics:
    timing = analysis.metrics.timing
    recognition = analysis.metrics.recognition
    e2e = analysis.metrics.e2e
    noise_count = sum(
        case.classification == TEXT_NOISE for case in analysis.gt_cases
    )
    empty_count = sum(
        case.classification == TEXT_EMPTY for case in analysis.gt_cases
    )
    return PaddleGateMetrics(
        timing_f1=timing.timing_f1,
        timing_precision=timing.timing_precision,
        timing_recall=timing.timing_recall,
        cer_macro=recognition.cer_macro,
        cer_micro=recognition.cer_micro,
        usable_recall=e2e.usable_subtitle_recall,
        noise_count=noise_count,
        empty_count=empty_count,
        line_box_mean_iou=box_mean_iou,
        line_box_recall_at_05=box_recall,
    )


def _shift_entries(
    entries: list[SrtEntry],
    *,
    offset_ms: int,
    start_index: int,
) -> list[SrtEntry]:
    return [
        SrtEntry(
            index=start_index + index,
            start_ms=entry.start_ms + offset_ms,
            end_ms=entry.end_ms + offset_ms,
            text=entry.text,
        )
        for index, entry in enumerate(entries)
    ]


def _box_from_json(value: dict[str, Any]) -> BoundingBox:
    return BoundingBox(
        x=int(value["x"]),
        y=int(value["y"]),
        width=int(value["width"]),
        height=int(value["height"]),
    )


def _measure_live_q0_boxes() -> tuple[float, float, dict[str, Any]]:
    live = build_live_report(report_with_timings=False)
    total_oracle = 0
    matched = 0
    ious: list[float] = []
    case_results: list[dict[str, Any]] = []
    for case in live["cases"]:
        oracle_lines = case["oracle"]["stages"]["10_output"]["lines"]
        candidate_lines = case["candidate"]["stages"]["10_output"]["lines"]
        total_oracle += len(oracle_lines)
        used: set[int] = set()
        case_ious: list[float] = []
        for oracle in oracle_lines:
            best_iou = 0.0
            best_index: int | None = None
            for index, candidate in enumerate(candidate_lines):
                if index in used or candidate["text"] != oracle["text"]:
                    continue
                iou = compute_box_iou(
                    _box_from_json(oracle["box"]),
                    _box_from_json(candidate["box"]),
                )
                if iou > best_iou:
                    best_iou = iou
                    best_index = index
            if best_index is not None:
                used.add(best_index)
                case_ious.append(best_iou)
                ious.append(best_iou)
                if best_iou >= 0.5:
                    matched += 1
        case_results.append(
            {
                "case_id": case["case_id"],
                "oracle_lines": len(oracle_lines),
                "candidate_lines": len(candidate_lines),
                "ious": case_ious,
            }
        )
    recall = matched / total_oracle if total_oracle else 1.0
    mean_iou = sum(ious) / len(ious) if ious else 1.0
    return mean_iou, recall, {
        "fixture_count": len(case_results),
        "oracle_line_count": total_oracle,
        "matched_at_iou_0_5": matched,
        "mean_iou": mean_iou,
        "recall_at_iou_0_5": recall,
        "cases": case_results,
    }


def _absolute_gate_results(
    baseline_metrics: PaddleGateMetrics,
    candidate: PaddleGateMetrics,
) -> list[GateEvaluationResult]:
    relative = evaluate_paddle_hard_gates(
        baseline_metrics,
        candidate,
    ).gate_results
    return [
        GateEvaluationResult(
            passed=item.passed,
            name=f"absolute_{item.name}",
            oracle_val=item.oracle_val,
            candidate_val=item.candidate_val,
            delta=item.delta,
            allowed_limit=item.allowed_limit,
            message=item.message,
        )
        for item in relative
    ]


def _report_payload(report: PaddleGateReport) -> dict[str, Any]:
    return {
        "schema_version": 1,
        "kind": "paddle_e2e_quality_gate",
        "overall_passed": report.overall_passed,
        "oracle": asdict(report.metrics_oracle),
        "candidate": asdict(report.metrics_candidate),
        "relative_gates": [asdict(item) for item in report.gate_results],
        "absolute_gates": [
            asdict(item) for item in report.absolute_gate_results
        ],
        "sources": report.source_results,
        "fingerprint": report.fingerprint,
    }


def generate_markdown_report(report: PaddleGateReport) -> str:
    """Format the measured gate report as Markdown."""
    lines = [
        "# SubLift Phase 6.8 — Paddle E2E Quality Gate Report",
        "",
        (
            "- **Overall Status**: PASS ✅"
            if report.overall_passed
            else "- **Overall Status**: FAIL ❌"
        ),
        "- **Engine**: `paddle`",
        "- **Oracle**: `runtime=python`",
        "- **Candidate**: `runtime=cpp`",
        "",
        "## Hard gates",
        "",
        "| Gate | Oracle | Candidate | Delta | Limit | Status |",
        "|---|---:|---:|---:|---:|---|",
    ]
    for item in [*report.gate_results, *report.absolute_gate_results]:
        lines.append(
            f"| `{item.name}` | {item.oracle_val:.5f} | "
            f"{item.candidate_val:.5f} | {item.delta:+.5f} | "
            f"{item.allowed_limit:+.5f} | "
            f"{'PASS' if item.passed else 'FAIL'} |"
        )
    if report.source_results:
        lines.extend(
            [
                "",
                "## Measured sources",
                "",
                "| Source | Duration | Python wall | C++ wall | Relative quality |",
                "|---|---:|---:|---:|---|",
            ]
        )
        for source in report.source_results:
            lines.append(
                f"| `{source['id']}` | "
                f"{float(source['duration_seconds']):.3f}s | "
                f"{float(source['oracle_wall_seconds']):.3f}s | "
                f"{float(source['candidate_wall_seconds']):.3f}s | "
                f"{'PASS' if source['relative_passed'] else 'FAIL'} |"
            )
    return "\n".join(lines) + "\n"


def _write_outputs(
    report: PaddleGateReport,
    *,
    report_out: Path | None,
    json_out: Path | None,
) -> None:
    if report_out is not None:
        report_out.parent.mkdir(parents=True, exist_ok=True)
        report_out.write_text(
            generate_markdown_report(report),
            encoding="utf-8",
        )
    if json_out is not None:
        json_out.parent.mkdir(parents=True, exist_ok=True)
        json_out.write_text(
            json.dumps(
                _report_payload(report),
                indent=2,
                ensure_ascii=False,
            )
            + "\n",
            encoding="utf-8",
        )


def _failed_availability_report(message: str) -> PaddleGateReport:
    gate = GateEvaluationResult(
        name="cpp_paddle_availability",
        passed=False,
        oracle_val=1.0,
        candidate_val=0.0,
        delta=-1.0,
        allowed_limit=0.0,
        message=message,
    )
    return PaddleGateReport(
        overall_passed=False,
        metrics_oracle=PaddleGateMetrics(),
        metrics_candidate=PaddleGateMetrics(),
        gate_results=[gate],
    )


def run_paddle_gate(
    check: bool = False,
    report_out: Path | None = None,
    json_out: Path | None = None,
    skip_runtime: bool = False,
    *,
    manifest_path: Path = DEFAULT_MANIFEST,
    baseline_path: Path = DEFAULT_BASELINE,
    freeze_oracle: bool = False,
) -> PaddleGateReport:
    """Execute all real Q2 product runs and evaluate relative/absolute gates."""
    if skip_runtime:
        report = _failed_availability_report(
            "Candidate C++ Paddle runtime was explicitly skipped"
        )
        _write_outputs(report, report_out=report_out, json_out=json_out)
        return report
    if not probe_cpp_paddle_available(repo_root=REPO_ROOT):
        report = _failed_availability_report(
            "Candidate C++ Paddle worker or model is unavailable"
        )
        _write_outputs(report, report_out=report_out, json_out=json_out)
        return report

    manifest = _load_and_validate_manifest(manifest_path)
    manifest_sha = _sha256(manifest_path)
    model_freeze = cast(
        dict[str, Any],
        json.loads(FREEZE_MANIFEST.read_text(encoding="utf-8")),
    )

    with tempfile.TemporaryDirectory(prefix="sublift-paddle-quality-") as tmp:
        temp_root = Path(tmp)
        oracle_all: list[SrtEntry] = []
        candidate_all: list[SrtEntry] = []
        ground_truth_all: list[SrtEntry] = []
        offset_ms = 0
        next_index = 1
        source_results: list[dict[str, Any]] = []
        per_source_oracle: dict[str, dict[str, Any]] = {}

        for source in manifest["sources"]:
            source_id = str(source["id"])
            oracle = _run_product(
                source,
                runtime="python",
                output_path=temp_root / f"{source_id}.python.srt",
            )
            candidate = _run_product(
                source,
                runtime="cpp",
                output_path=temp_root / f"{source_id}.cpp.srt",
            )
            ground_truth = load_srt(
                _resolve_path(str(source["ground_truth"]))
            )
            oracle_analysis = analyze_entries(
                oracle.entries,
                ground_truth,
                temporal_iou_threshold=0.5,
            )
            candidate_analysis = analyze_entries(
                candidate.entries,
                ground_truth,
                temporal_iou_threshold=0.5,
            )
            oracle_metrics = _metrics_from_analysis(oracle_analysis)
            candidate_metrics = _metrics_from_analysis(candidate_analysis)
            source_gate = evaluate_paddle_hard_gates(
                oracle_metrics,
                candidate_metrics,
            )
            per_clip_noise_empty = (
                candidate_metrics.noise_count <= oracle_metrics.noise_count + 1
                and candidate_metrics.empty_count
                <= oracle_metrics.empty_count + 1
            )
            source_relative_passed = (
                source_gate.overall_passed
                and per_clip_noise_empty
                and oracle.output_sha256 == candidate.output_sha256
            )
            source_results.append(
                {
                    "id": source_id,
                    "kind": source["kind"],
                    "tags": source["tags"],
                    "duration_seconds": source["duration_seconds"],
                    "video_sha256": source["video_sha256"],
                    "ground_truth_sha256": source["ground_truth_sha256"],
                    "oracle_wall_seconds": oracle.elapsed_seconds,
                    "candidate_wall_seconds": candidate.elapsed_seconds,
                    "oracle_output_sha256": oracle.output_sha256,
                    "candidate_output_sha256": candidate.output_sha256,
                    "output_sha256_exact": (
                        oracle.output_sha256 == candidate.output_sha256
                    ),
                    "oracle_metrics": asdict(oracle_metrics),
                    "candidate_metrics": asdict(candidate_metrics),
                    "relative_passed": source_relative_passed,
                    "per_clip_noise_empty_passed": per_clip_noise_empty,
                    "relative_gates": [
                        asdict(item) for item in source_gate.gate_results
                    ],
                }
            )
            per_source_oracle[source_id] = asdict(oracle_metrics)

            oracle_shifted = _shift_entries(
                oracle.entries,
                offset_ms=offset_ms,
                start_index=next_index,
            )
            candidate_shifted = _shift_entries(
                candidate.entries,
                offset_ms=offset_ms,
                start_index=next_index,
            )
            gt_shifted = _shift_entries(
                ground_truth,
                offset_ms=offset_ms,
                start_index=next_index,
            )
            oracle_all.extend(oracle_shifted)
            candidate_all.extend(candidate_shifted)
            ground_truth_all.extend(gt_shifted)
            next_index += max(
                len(oracle_shifted),
                len(candidate_shifted),
                len(gt_shifted),
            )
            offset_ms += (
                int(float(source["duration_seconds"]) * 1000.0) + 10_000
            )

        box_mean_iou, box_recall, box_details = _measure_live_q0_boxes()
        oracle_aggregate = _metrics_from_analysis(
            analyze_entries(
                oracle_all,
                ground_truth_all,
                temporal_iou_threshold=0.5,
            )
        )
        candidate_aggregate = _metrics_from_analysis(
            analyze_entries(
                candidate_all,
                ground_truth_all,
                temporal_iou_threshold=0.5,
            ),
            box_mean_iou=box_mean_iou,
            box_recall=box_recall,
        )

    if freeze_oracle:
        baseline = {
            "schema_version": 1,
            "kind": "paddle_quality_oracle_baseline",
            "oracle_commit": _git_head(),
            "dataset_manifest_sha256": manifest_sha,
            "model_fingerprint": model_freeze,
            "oracle": asdict(oracle_aggregate),
            "per_source_oracle": per_source_oracle,
        }
        baseline_path.parent.mkdir(parents=True, exist_ok=True)
        baseline_path.write_text(
            json.dumps(baseline, indent=2, ensure_ascii=False) + "\n",
            encoding="utf-8",
        )
    if not baseline_path.is_file():
        raise RuntimeError(
            f"Paddle quality Oracle baseline is missing: {baseline_path}; "
            "run once with --freeze-oracle"
        )
    baseline = cast(
        dict[str, Any],
        json.loads(baseline_path.read_text(encoding="utf-8")),
    )
    if baseline.get("dataset_manifest_sha256") != manifest_sha:
        raise RuntimeError("Paddle quality Oracle baseline dataset is stale")
    if baseline.get("model_fingerprint") != model_freeze:
        raise RuntimeError("Paddle quality Oracle baseline model fingerprint is stale")

    relative = evaluate_paddle_hard_gates(
        oracle_aggregate,
        candidate_aggregate,
    )
    source_hash_gate = evaluate_source_output_hashes(source_results)
    relative.gate_results.append(source_hash_gate)
    relative.overall_passed = (
        relative.overall_passed and source_hash_gate.passed
    )
    baseline_oracle = cast(dict[str, Any], baseline["oracle"])
    baseline_metrics = PaddleGateMetrics(**baseline_oracle)
    absolute_gates = _absolute_gate_results(
        baseline_metrics,
        candidate_aggregate,
    )
    all_sources_passed = all(
        bool(source["relative_passed"]) for source in source_results
    )
    overall = (
        relative.overall_passed
        and all(item.passed for item in absolute_gates)
        and all_sources_passed
    )
    report = PaddleGateReport(
        overall_passed=overall,
        metrics_oracle=oracle_aggregate,
        metrics_candidate=candidate_aggregate,
        gate_results=relative.gate_results,
        absolute_gate_results=absolute_gates,
        source_results=source_results,
        fingerprint={
            "engine": "paddle",
            "oracle_runtime": "python",
            "candidate_runtime": "cpp",
            "candidate_commit": _git_head(),
            "dataset_id": manifest["dataset_id"],
            "dataset_manifest_sha256": manifest_sha,
            "source_count": len(manifest["sources"]),
            "total_duration_seconds": sum(
                float(item["duration_seconds"])
                for item in manifest["sources"]
            ),
            "model": model_freeze["model"],
            "parameters": model_freeze["parameters"],
            "box_gate": box_details,
            "oracle_baseline_path": str(baseline_path),
            "oracle_baseline_sha256": _sha256(baseline_path),
        },
    )
    _write_outputs(report, report_out=report_out, json_out=json_out)
    if check and not report.overall_passed:
        return report
    return report


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Check measured multi-source Paddle E2E quality"
    )
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--skip-runtime", action="store_true")
    parser.add_argument("--freeze-oracle", action="store_true")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--baseline", type=Path, default=DEFAULT_BASELINE)
    parser.add_argument("--report-out", type=Path, default=None)
    parser.add_argument("--json-out", type=Path, default=None)
    args = parser.parse_args()

    report = run_paddle_gate(
        check=args.check,
        report_out=args.report_out,
        json_out=args.json_out,
        skip_runtime=args.skip_runtime,
        manifest_path=args.manifest,
        baseline_path=args.baseline,
        freeze_oracle=args.freeze_oracle,
    )
    print(
        "[Paddle Quality Gate] Status: "
        f"{'PASS' if report.overall_passed else 'FAIL'}"
    )
    if args.check and not report.overall_passed:
        failures = [
            item
            for item in [
                *report.gate_results,
                *report.absolute_gate_results,
            ]
            if not item.passed
        ]
        for failure in failures:
            print(
                f"[Paddle Quality Gate] {failure.name}: {failure.message}",
                file=sys.stderr,
            )
        raise SystemExit(1)


if __name__ == "__main__":
    main()
