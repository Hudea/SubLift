"""Benchmark diagnostics and agent-readable report payloads.

This layer keeps the existing benchmark runner intact, but adds stricter
diagnostics: one-to-one temporal matching, timing/recognition/e2e metrics,
per-GT and per-detection case classifications, and stable report helpers.
"""

from __future__ import annotations

import csv
import io
import json
import math
import unicodedata
from collections import Counter
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import TYPE_CHECKING

from benchmark.srt_loader import SrtEntry

if TYPE_CHECKING:
    from benchmark.runner import RunResult


DEFAULT_USABLE_CER_THRESHOLD = 0.20
DEFAULT_SHORT_SUBTITLE_THRESHOLD_MS = 1500
TIMING_F1_GATE = 0.95
USABLE_RECALL_GATE = 0.80

OK = "ok"
FN_NO_OVERLAP = "timing.fn.no_overlap"
FN_MERGED = "timing.fn.merged_into_neighbor"
FN_BOUNDARY = "timing.fn.boundary_miss"
FP_FALSE_ALARM = "timing.fp.false_alarm"
FP_SPLIT_EXTRA = "timing.fp.split_extra"
TEXT_EMPTY = "text.empty"
TEXT_HIGH_CER = "text.high_cer"
TEXT_NOISE = "text.noise"


@dataclass(frozen=True)
class TemporalMatch:
    """One-to-one temporal match between one GT entry and one detection."""

    gt_id: int
    det_id: int
    overlap_ms: int
    temporal_iou: float


@dataclass(frozen=True)
class GtCase:
    """Diagnostic row for one ground-truth subtitle."""

    gt_id: int
    gt_start_ms: int
    gt_end_ms: int
    gt_text: str
    matched_det_id: int | None
    overlap_ms: int
    temporal_iou: float
    start_error_ms: int | None
    end_error_ms: int | None
    cer: float | None
    exact_match: bool | None
    usable: bool
    classification: str
    failure_type: str
    notes: str


@dataclass(frozen=True)
class DetCase:
    """Diagnostic row for one detected subtitle segment."""

    det_id: int
    det_start_ms: int
    det_end_ms: int
    det_text: str
    matched_gt_id: int | None
    overlap_ms: int
    temporal_iou: float
    classification: str
    failure_type: str
    notes: str


@dataclass(frozen=True)
class TimingMetrics:
    """Timing-only metrics from one-to-one temporal matching."""

    timing_recall: float
    timing_precision: float
    timing_f1: float
    start_mae_ms: float
    end_mae_ms: float
    boundary_p95_ms: float
    miss_count: int
    split_count: int
    merge_count: int
    false_alarm_count: int


@dataclass(frozen=True)
class RecognitionMetrics:
    """OCR text metrics evaluated only on temporally matched subtitles."""

    cer_macro: float
    cer_micro: float
    char_accuracy: float
    exact_match_rate: float
    empty_text_rate: float
    text_eval_coverage: float


@dataclass(frozen=True)
class E2EMetrics:
    """End-to-end usability metric."""

    usable_subtitle_recall: float
    usable_count: int
    usable_cer_threshold: float


@dataclass(frozen=True)
class BenchmarkDiagnosticMetrics:
    """Complete benchmark diagnostic metrics bundle."""

    timing: TimingMetrics
    recognition: RecognitionMetrics
    e2e: E2EMetrics


@dataclass(frozen=True)
class BenchmarkDiagnostics:
    """Agent-readable benchmark analysis computed from a run result."""

    metrics: BenchmarkDiagnosticMetrics
    gt_cases: list[GtCase]
    det_cases: list[DetCase]
    gates: list[dict[str, object]]
    summary: dict[str, object]
    failure_clusters: list[dict[str, object]]
    thresholds: dict[str, object]


def analyze_result(
    result: RunResult,
    *,
    baseline_timing_precision: float | None = None,
) -> BenchmarkDiagnostics:
    """Compute benchmark diagnostics from a runner result."""
    return analyze_entries(
        result.detected,
        result.ground_truth,
        temporal_iou_threshold=result.config.match_threshold,
        baseline_timing_precision=baseline_timing_precision,
    )


def analyze_entries(
    detected: list[SrtEntry],
    ground_truth: list[SrtEntry],
    *,
    temporal_iou_threshold: float = 0.5,
    usable_cer_threshold: float = DEFAULT_USABLE_CER_THRESHOLD,
    short_subtitle_threshold_ms: int = DEFAULT_SHORT_SUBTITLE_THRESHOLD_MS,
    baseline_timing_precision: float | None = None,
) -> BenchmarkDiagnostics:
    """Analyze detected/GT entries with one-to-one matching semantics."""
    matches = _match_one_to_one(detected, ground_truth, threshold=temporal_iou_threshold)
    match_by_gt = {m.gt_id: m for m in matches}
    match_by_det = {m.det_id: m for m in matches}

    gt_by_id = {gt.index: gt for gt in ground_truth}
    det_by_id = {det.index: det for det in detected}

    gt_cases = _build_gt_cases(
        detected,
        ground_truth,
        match_by_gt,
        det_by_id,
        temporal_iou_threshold=temporal_iou_threshold,
        usable_cer_threshold=usable_cer_threshold,
    )
    det_cases = _build_det_cases(
        detected,
        ground_truth,
        match_by_det,
        gt_by_id,
    )
    metrics = _compute_metrics(
        gt_cases,
        det_cases,
        detected_count=len(detected),
        gt_count=len(ground_truth),
        usable_cer_threshold=usable_cer_threshold,
    )
    gates = _build_gates(metrics, baseline_timing_precision=baseline_timing_precision)
    failure_clusters = _build_failure_clusters(gt_cases, det_cases)
    summary = _build_summary(metrics, gates, failure_clusters)

    return BenchmarkDiagnostics(
        metrics=metrics,
        gt_cases=gt_cases,
        det_cases=det_cases,
        gates=gates,
        summary=summary,
        failure_clusters=failure_clusters,
        thresholds={
            "temporal_iou": temporal_iou_threshold,
            "usable_cer": usable_cer_threshold,
            "short_subtitle_ms": short_subtitle_threshold_ms,
        },
    )


def format_agent_json(
    result: RunResult,
    analysis: BenchmarkDiagnostics,
    *,
    artifacts: dict[str, Path] | None = None,
) -> str:
    """Format diagnostics as stable agent-readable JSON."""
    payload = {
        "run": {
            "video": str(result.config.video_path),
            "ground_truth": str(result.config.ground_truth_path),
            "fps": result.config.fps,
            "engine": result.config.engine,
            "confidence": result.config.confidence,
            "temporal_iou_threshold": result.config.match_threshold,
            "region_box": list(result.config.region_box) if result.config.region_box else None,
            "detector": "fixed_region" if result.config.region_box else "bottom_crop",
            "label": result.config.label,
            "exported_srt": str(result.exported_srt_path) if result.exported_srt_path else None,
        },
        "status": analysis.summary["status"],
        "summary": analysis.summary,
        "gates": analysis.gates,
        "metrics": asdict(analysis.metrics),
        "speed": _speed_payload(result),
        "thresholds": analysis.thresholds,
        "counts": {
            "ground_truth": len(result.ground_truth),
            "detected": len(result.detected),
            "gt_cases": len(analysis.gt_cases),
            "det_cases": len(analysis.det_cases),
        },
        "failure_clusters": analysis.failure_clusters,
        "cases": {
            "gt_cases": [asdict(case) for case in analysis.gt_cases],
            "det_cases": [asdict(case) for case in analysis.det_cases],
        },
        "repro_command": "uv run python scripts/run_benchmark_manifest.py <manifest.json>",
        "artifacts": _artifact_payload(artifacts or {}),
    }
    return json.dumps(payload, ensure_ascii=False, indent=2)


def format_gt_cases_csv(analysis: BenchmarkDiagnostics) -> str:
    """Format GT diagnostic cases as CSV."""
    output = io.StringIO()
    writer = csv.writer(output)
    writer.writerow(
        [
            "gt_id",
            "gt_start_ms",
            "gt_end_ms",
            "gt_text",
            "matched_det_id",
            "overlap_ms",
            "temporal_iou",
            "start_error_ms",
            "end_error_ms",
            "cer",
            "exact_match",
            "usable",
            "classification",
            "failure_type",
            "notes",
        ]
    )
    for case in analysis.gt_cases:
        writer.writerow(
            [
                case.gt_id,
                case.gt_start_ms,
                case.gt_end_ms,
                case.gt_text,
                case.matched_det_id or "",
                case.overlap_ms,
                _fmt_float(case.temporal_iou),
                case.start_error_ms if case.start_error_ms is not None else "",
                case.end_error_ms if case.end_error_ms is not None else "",
                _fmt_float(case.cer) if case.cer is not None else "",
                case.exact_match if case.exact_match is not None else "",
                case.usable,
                case.classification,
                case.failure_type,
                case.notes,
            ]
        )
    return output.getvalue()


def format_det_cases_csv(analysis: BenchmarkDiagnostics) -> str:
    """Format detection diagnostic cases as CSV."""
    output = io.StringIO()
    writer = csv.writer(output)
    writer.writerow(
        [
            "det_id",
            "det_start_ms",
            "det_end_ms",
            "det_text",
            "matched_gt_id",
            "overlap_ms",
            "temporal_iou",
            "classification",
            "failure_type",
            "notes",
        ]
    )
    for case in analysis.det_cases:
        writer.writerow(
            [
                case.det_id,
                case.det_start_ms,
                case.det_end_ms,
                case.det_text,
                case.matched_gt_id or "",
                case.overlap_ms,
                _fmt_float(case.temporal_iou),
                case.classification,
                case.failure_type,
                case.notes,
            ]
        )
    return output.getvalue()


def format_summary_markdown(
    result: RunResult,
    analysis: BenchmarkDiagnostics,
    *,
    artifacts: dict[str, Path] | None = None,
) -> str:
    """Format a compact human-readable diagnostic summary."""
    timing = analysis.metrics.timing
    recognition = analysis.metrics.recognition
    e2e = analysis.metrics.e2e
    lines: list[str] = [f"# Benchmark Summary: {result.config.output_prefix}", ""]

    lines.append("## Conclusion")
    lines.append(f"- conclusion: {analysis.summary['conclusion']}")
    lines.append(f"- status: `{analysis.summary['status']}`")
    lines.append(f"- main_improvement: `{analysis.summary['main_improvement']}`")
    lines.append(f"- primary_remaining_gap: `{analysis.summary['primary_remaining_gap']}`")
    lines.append(f"- primary_regression: `{analysis.summary['primary_regression']}`")
    lines.append("")

    lines.append("## Metrics")
    lines.append("| Group | Metric | Value |")
    lines.append("|---|---|---|")
    lines.append(f"| timing | timing_recall | {timing.timing_recall * 100:.1f}% |")
    lines.append(f"| timing | timing_precision | {timing.timing_precision * 100:.1f}% |")
    lines.append(f"| timing | timing_f1 | {timing.timing_f1 * 100:.1f}% |")
    lines.append(f"| timing | start_mae_ms | {timing.start_mae_ms:.1f} |")
    lines.append(f"| timing | end_mae_ms | {timing.end_mae_ms:.1f} |")
    lines.append(f"| timing | boundary_p95_ms | {timing.boundary_p95_ms:.1f} |")
    lines.append(f"| recognition | cer_macro | {recognition.cer_macro * 100:.1f}% |")
    lines.append(f"| recognition | cer_micro | {recognition.cer_micro * 100:.1f}% |")
    lines.append(f"| recognition | char_accuracy | {recognition.char_accuracy * 100:.1f}% |")
    lines.append(f"| recognition | exact_match_rate | {recognition.exact_match_rate * 100:.1f}% |")
    lines.append(f"| recognition | empty_text_rate | {recognition.empty_text_rate * 100:.1f}% |")
    lines.append(f"| e2e | usable_subtitle_recall | {e2e.usable_subtitle_recall * 100:.1f}% |")
    speed = _speed_payload(result)
    lines.append(f"| speed | elapsed_seconds | {speed['elapsed_seconds']:.1f} |")
    lines.append(f"| speed | video_duration_seconds | {speed['video_duration_seconds']:.1f} |")
    lines.append(f"| speed | speed_factor | {speed['speed_factor']:.1f}x |")
    lines.append("")

    lines.append("## Failure Clusters")
    if not analysis.failure_clusters:
        lines.append("- (none)")
    else:
        lines.append("| Type | Count | Examples | Suggested action |")
        lines.append("|---|---:|---|---|")
        for cluster in analysis.failure_clusters:
            examples_value = cluster["examples"]
            examples = (
                ", ".join(str(item) for item in examples_value)
                if isinstance(examples_value, list)
                else ""
            )
            lines.append(
                f"| `{cluster['type']}` | {cluster['count']} | {examples} | "
                f"{cluster['suggested_action']} |"
            )
    lines.append("")

    if artifacts:
        lines.append("## Artifacts")
        for kind, path in artifacts.items():
            lines.append(f"- {kind}: `{path}`")
        lines.append("")

    return "\n".join(lines)


def _match_one_to_one(
    detected: list[SrtEntry],
    ground_truth: list[SrtEntry],
    *,
    threshold: float,
) -> list[TemporalMatch]:
    candidates: list[tuple[float, int, int, int, int]] = []
    for det in detected:
        for gt in ground_truth:
            ov = _overlap_ms(det.start_ms, det.end_ms, gt.start_ms, gt.end_ms)
            if ov <= 0:
                continue
            iou = _temporal_iou(det, gt, ov)
            if iou <= 0:
                continue
            candidates.append((iou, ov, det.index, gt.index, abs(det.start_ms - gt.start_ms)))

    candidates.sort(key=lambda item: (-item[0], -item[1], item[4], item[2], item[3]))
    assigned_det: set[int] = set()
    assigned_gt: set[int] = set()
    matches: list[TemporalMatch] = []
    for iou, ov, det_id, gt_id, _ in candidates:
        if iou < threshold or det_id in assigned_det or gt_id in assigned_gt:
            continue
        assigned_det.add(det_id)
        assigned_gt.add(gt_id)
        matches.append(
            TemporalMatch(gt_id=gt_id, det_id=det_id, overlap_ms=ov, temporal_iou=iou)
        )
    return sorted(matches, key=lambda item: item.gt_id)


def _build_gt_cases(
    detected: list[SrtEntry],
    ground_truth: list[SrtEntry],
    match_by_gt: dict[int, TemporalMatch],
    det_by_id: dict[int, SrtEntry],
    *,
    temporal_iou_threshold: float,
    usable_cer_threshold: float,
) -> list[GtCase]:
    cases: list[GtCase] = []
    for gt in ground_truth:
        match = match_by_gt.get(gt.index)
        if match is not None:
            det = det_by_id[match.det_id]
            cer = _cer(det.text, gt.text)
            exact_match = _normalize_text(det.text) == _normalize_text(gt.text)
            classification, failure_type, notes = _classify_text(det.text, gt.text, cer)
            usable = cer is not None and cer <= usable_cer_threshold and classification == OK
            cases.append(
                GtCase(
                    gt_id=gt.index,
                    gt_start_ms=gt.start_ms,
                    gt_end_ms=gt.end_ms,
                    gt_text=gt.text,
                    matched_det_id=det.index,
                    overlap_ms=match.overlap_ms,
                    temporal_iou=match.temporal_iou,
                    start_error_ms=det.start_ms - gt.start_ms,
                    end_error_ms=det.end_ms - gt.end_ms,
                    cer=cer,
                    exact_match=exact_match,
                    usable=usable,
                    classification=classification,
                    failure_type=failure_type,
                    notes=notes,
                )
            )
            continue

        best = _best_overlap_for_gt(gt, detected)
        if best is None:
            classification = FN_NO_OVERLAP
            overlap = 0
            iou = 0.0
            notes = "no detection overlaps this ground-truth subtitle"
        else:
            related_det, overlap, iou = best
            neighbor_count = _overlapping_gt_count(related_det, ground_truth)
            if neighbor_count >= 2:
                classification = FN_MERGED
                notes = (
                    f"related detection #{related_det.index} also overlaps "
                    f"{neighbor_count} GT entries"
                )
            else:
                classification = FN_BOUNDARY
                notes = (
                    f"related detection #{related_det.index} exists but temporal_iou "
                    f"{iou:.3f} is below threshold {temporal_iou_threshold:.3f}"
                )

        cases.append(
            GtCase(
                gt_id=gt.index,
                gt_start_ms=gt.start_ms,
                gt_end_ms=gt.end_ms,
                gt_text=gt.text,
                matched_det_id=None,
                overlap_ms=overlap,
                temporal_iou=iou,
                start_error_ms=None,
                end_error_ms=None,
                cer=None,
                exact_match=None,
                usable=False,
                classification=classification,
                failure_type=classification,
                notes=notes,
            )
        )
    return cases


def _build_det_cases(
    detected: list[SrtEntry],
    ground_truth: list[SrtEntry],
    match_by_det: dict[int, TemporalMatch],
    gt_by_id: dict[int, SrtEntry],
) -> list[DetCase]:
    matched_gt_ids = {match.gt_id for match in match_by_det.values()}
    cases: list[DetCase] = []
    for det in detected:
        match = match_by_det.get(det.index)
        if match is not None:
            cases.append(
                DetCase(
                    det_id=det.index,
                    det_start_ms=det.start_ms,
                    det_end_ms=det.end_ms,
                    det_text=det.text,
                    matched_gt_id=match.gt_id,
                    overlap_ms=match.overlap_ms,
                    temporal_iou=match.temporal_iou,
                    classification=OK,
                    failure_type="",
                    notes=f"matched GT #{match.gt_id}",
                )
            )
            continue

        best = _best_overlap_for_det(det, ground_truth)
        if best is not None and best[0].index in matched_gt_ids:
            gt, overlap, iou = best
            classification = FP_SPLIT_EXTRA
            notes = f"extra detection overlaps already matched GT #{gt.index}"
        elif best is not None:
            gt, overlap, iou = best
            classification = FP_FALSE_ALARM
            notes = f"overlaps unmatched GT #{gt.index} but was not a valid one-to-one match"
        else:
            overlap = 0
            iou = 0.0
            classification = FP_FALSE_ALARM
            notes = "no ground-truth subtitle overlaps this detection"

        cases.append(
            DetCase(
                det_id=det.index,
                det_start_ms=det.start_ms,
                det_end_ms=det.end_ms,
                det_text=det.text,
                matched_gt_id=None,
                overlap_ms=overlap,
                temporal_iou=iou,
                classification=classification,
                failure_type=classification,
                notes=notes,
            )
        )
    return cases


def _compute_metrics(
    gt_cases: list[GtCase],
    det_cases: list[DetCase],
    *,
    detected_count: int,
    gt_count: int,
    usable_cer_threshold: float,
) -> BenchmarkDiagnosticMetrics:
    matched_gt = [case for case in gt_cases if case.matched_det_id is not None]
    matched_count = len(matched_gt)

    timing_recall = matched_count / gt_count if gt_count else 0.0
    timing_precision = matched_count / detected_count if detected_count else 0.0
    timing_f1 = (
        2 * timing_recall * timing_precision / (timing_recall + timing_precision)
        if timing_recall + timing_precision > 0
        else 0.0
    )

    start_errors = [
        abs(case.start_error_ms)
        for case in matched_gt
        if case.start_error_ms is not None
    ]
    end_errors = [
        abs(case.end_error_ms)
        for case in matched_gt
        if case.end_error_ms is not None
    ]
    boundary_errors = [*start_errors, *end_errors]

    text_cases = [case for case in matched_gt if case.cer is not None]
    edit_distance_sum = 0
    ref_len_sum = 0
    for case in matched_gt:
        if case.cer is None:
            continue
        gt_norm = _normalize_text(case.gt_text)
        det_norm = _normalize_text(_det_text_for_case(case, det_cases))
        edit_distance_sum += _levenshtein(det_norm, gt_norm)
        ref_len_sum += len(gt_norm)

    cer_macro = (
        sum(case.cer for case in text_cases if case.cer is not None) / len(text_cases)
        if text_cases
        else 0.0
    )
    cer_micro = edit_distance_sum / ref_len_sum if ref_len_sum else 0.0
    exact_match_rate = (
        sum(1 for case in text_cases if case.exact_match) / len(text_cases)
        if text_cases
        else 0.0
    )
    empty_text_rate = (
        sum(1 for case in matched_gt if case.classification == TEXT_EMPTY) / len(matched_gt)
        if matched_gt
        else 0.0
    )
    text_eval_coverage = len(text_cases) / gt_count if gt_count else 0.0
    usable_count = sum(1 for case in gt_cases if case.usable)

    return BenchmarkDiagnosticMetrics(
        timing=TimingMetrics(
            timing_recall=timing_recall,
            timing_precision=timing_precision,
            timing_f1=timing_f1,
            start_mae_ms=_mean(start_errors),
            end_mae_ms=_mean(end_errors),
            boundary_p95_ms=_p95(boundary_errors),
            miss_count=sum(1 for case in gt_cases if case.matched_det_id is None),
            split_count=sum(1 for case in det_cases if case.classification == FP_SPLIT_EXTRA),
            merge_count=sum(1 for case in gt_cases if case.classification == FN_MERGED),
            false_alarm_count=sum(1 for case in det_cases if case.classification == FP_FALSE_ALARM),
        ),
        recognition=RecognitionMetrics(
            cer_macro=cer_macro,
            cer_micro=cer_micro,
            char_accuracy=max(0.0, 1.0 - cer_micro),
            exact_match_rate=exact_match_rate,
            empty_text_rate=empty_text_rate,
            text_eval_coverage=text_eval_coverage,
        ),
        e2e=E2EMetrics(
            usable_subtitle_recall=usable_count / gt_count if gt_count else 0.0,
            usable_count=usable_count,
            usable_cer_threshold=usable_cer_threshold,
        ),
    )


def _build_gates(
    metrics: BenchmarkDiagnosticMetrics,
    *,
    baseline_timing_precision: float | None,
) -> list[dict[str, object]]:
    gates: list[dict[str, object]] = [
        {
            "metric": "timing_f1",
            "target": TIMING_F1_GATE,
            "actual": metrics.timing.timing_f1,
            "pass": metrics.timing.timing_f1 >= TIMING_F1_GATE,
            "blocking": True,
        }
    ]
    if baseline_timing_precision is None:
        gates.append(
            {
                "metric": "timing_precision",
                "target": "not_lower_than_baseline",
                "baseline": None,
                "actual": metrics.timing.timing_precision,
                "pass": None,
                "blocking": True,
                "status": "baseline_unavailable",
            }
        )
    else:
        gates.append(
            {
                "metric": "timing_precision",
                "target": "not_lower_than_baseline",
                "baseline": baseline_timing_precision,
                "actual": metrics.timing.timing_precision,
                "pass": metrics.timing.timing_precision >= baseline_timing_precision,
                "blocking": True,
            }
        )
    gates.append(
        {
            "metric": "usable_subtitle_recall",
            "target": USABLE_RECALL_GATE,
            "actual": metrics.e2e.usable_subtitle_recall,
            "pass": metrics.e2e.usable_subtitle_recall >= USABLE_RECALL_GATE,
            "blocking": False,
        }
    )
    return gates


def _build_summary(
    metrics: BenchmarkDiagnosticMetrics,
    gates: list[dict[str, object]],
    failure_clusters: list[dict[str, object]],
) -> dict[str, object]:
    blocking_failures = [
        gate for gate in gates if gate.get("blocking") is True and gate.get("pass") is False
    ]
    precision_gate = next(gate for gate in gates if gate["metric"] == "timing_precision")
    primary_regression = (
        "timing_precision_regression"
        if precision_gate.get("pass") is False
        else "baseline_unavailable"
        if precision_gate.get("pass") is None
        else "none"
    )
    primary_remaining_gap = (
        str(failure_clusters[0]["type"]) if failure_clusters else "none"
    )
    status = "fail" if blocking_failures else "pass"
    conclusion = (
        "timing_f1 gate failed; inspect failure_clusters for the next optimization target"
        if status == "fail"
        else "timing gates passed; inspect recognition and e2e metrics for residual quality work"
    )
    main_improvement = "baseline_unavailable"
    if metrics.timing.merge_count == 0 and metrics.timing.split_count == 0:
        main_improvement = "timing_alignment_clean"

    return {
        "status": status,
        "conclusion": conclusion,
        "main_improvement": main_improvement,
        "primary_remaining_gap": primary_remaining_gap,
        "primary_regression": primary_regression,
    }


def _build_failure_clusters(
    gt_cases: list[GtCase],
    det_cases: list[DetCase],
) -> list[dict[str, object]]:
    counter: Counter[str] = Counter()
    examples: dict[str, list[int]] = {}
    for gt_case in gt_cases:
        if gt_case.classification == OK:
            continue
        counter[gt_case.classification] += 1
        examples.setdefault(gt_case.classification, []).append(gt_case.gt_id)
    for det_case in det_cases:
        if det_case.classification == OK:
            continue
        counter[det_case.classification] += 1
        examples.setdefault(det_case.classification, []).append(det_case.det_id)

    clusters: list[dict[str, object]] = []
    for kind, count in counter.most_common():
        clusters.append(
            {
                "type": kind,
                "count": count,
                "examples": examples[kind][:10],
                "suggested_action": _suggested_action(kind),
            }
        )
    return clusters


def _classify_text(
    detected_text: str,
    gt_text: str,
    cer: float | None,
) -> tuple[str, str, str]:
    if not detected_text.strip():
        return TEXT_EMPTY, TEXT_EMPTY, "matched timing but OCR output is empty"
    if cer is None:
        return OK, "", "reference text is empty; text quality skipped"
    if cer <= DEFAULT_USABLE_CER_THRESHOLD:
        return OK, "", "timing and text are usable"

    det_norm = _normalize_text(detected_text)
    gt_norm = _normalize_text(gt_text)
    if gt_norm and len(det_norm) > len(gt_norm) * 1.5:
        return TEXT_NOISE, TEXT_NOISE, "detected text is much longer than reference"
    return TEXT_HIGH_CER, TEXT_HIGH_CER, "matched timing but text CER is high"


def _best_overlap_for_gt(
    gt: SrtEntry,
    detected: list[SrtEntry],
) -> tuple[SrtEntry, int, float] | None:
    best: tuple[SrtEntry, int, float] | None = None
    for det in detected:
        ov = _overlap_ms(gt.start_ms, gt.end_ms, det.start_ms, det.end_ms)
        if ov <= 0:
            continue
        iou = _temporal_iou(det, gt, ov)
        if best is None or ov > best[1] or (ov == best[1] and iou > best[2]):
            best = (det, ov, iou)
    return best


def _best_overlap_for_det(
    det: SrtEntry,
    ground_truth: list[SrtEntry],
) -> tuple[SrtEntry, int, float] | None:
    best: tuple[SrtEntry, int, float] | None = None
    for gt in ground_truth:
        ov = _overlap_ms(det.start_ms, det.end_ms, gt.start_ms, gt.end_ms)
        if ov <= 0:
            continue
        iou = _temporal_iou(det, gt, ov)
        if best is None or ov > best[1] or (ov == best[1] and iou > best[2]):
            best = (gt, ov, iou)
    return best


def _overlapping_gt_count(det: SrtEntry, ground_truth: list[SrtEntry]) -> int:
    return sum(
        1
        for gt in ground_truth
        if _overlap_ms(det.start_ms, det.end_ms, gt.start_ms, gt.end_ms) > 0
    )


def _temporal_iou(det: SrtEntry, gt: SrtEntry, overlap: int) -> float:
    det_dur = max(0, det.end_ms - det.start_ms)
    gt_dur = max(0, gt.end_ms - gt.start_ms)
    union = det_dur + gt_dur - overlap
    return overlap / union if union > 0 else 0.0


def _overlap_ms(a_start: int, a_end: int, b_start: int, b_end: int) -> int:
    """Calculate temporal interval overlap in milliseconds."""
    return max(0, min(a_end, b_end) - max(a_start, b_start))


def _cer(detected: str, reference: str) -> float | None:
    ref_norm = _normalize_text(reference)
    if not ref_norm:
        return None
    det_norm = _normalize_text(detected)
    return _levenshtein(det_norm, ref_norm) / len(ref_norm)


def _det_text_for_case(case: GtCase, det_cases: list[DetCase]) -> str:
    if case.matched_det_id is None:
        return ""
    for det_case in det_cases:
        if det_case.det_id == case.matched_det_id:
            return det_case.det_text
    return ""


def _normalize_text(text: str) -> str:
    return "".join(unicodedata.normalize("NFKC", text).split())


def _levenshtein(s1: str, s2: str) -> int:
    if len(s1) < len(s2):
        s1, s2 = s2, s1
    if not s2:
        return len(s1)
    prev = list(range(len(s2) + 1))
    for i, c1 in enumerate(s1):
        curr = [i + 1]
        for j, c2 in enumerate(s2):
            curr.append(
                min(
                    prev[j + 1] + 1,
                    curr[j] + 1,
                    prev[j] + (c1 != c2),
                )
            )
        prev = curr
    return prev[-1]


def _mean(values: list[int]) -> float:
    return sum(values) / len(values) if values else 0.0


def _p95(values: list[int]) -> float:
    if not values:
        return 0.0
    sorted_values = sorted(values)
    index = max(0, math.ceil(len(sorted_values) * 0.95) - 1)
    return float(sorted_values[index])


def _suggested_action(kind: str) -> str:
    actions = {
        FN_NO_OVERLAP: "inspect region, sampling fps, and presence detection thresholds",
        FN_MERGED: "improve CHANGE detection or split long stable segments",
        FN_BOUNDARY: "tune hysteresis and boundary snapping",
        FP_FALSE_ALARM: "tighten foreground detection or OCR confidence filtering",
        FP_SPLIT_EXTRA: "add fuzzy adjacent merge after patrol-triggered splits",
        TEXT_EMPTY: "check OCR anchor frame selection and confidence threshold",
        TEXT_HIGH_CER: "improve OCR region quality or text normalization",
        TEXT_NOISE: "tighten crop region or filter non-subtitle text",
    }
    return actions.get(kind, "inspect matching cases and trace output")


def _fmt_float(value: float | None) -> str:
    if value is None:
        return ""
    return f"{value:.6f}"


def _artifact_payload(artifacts: dict[str, Path]) -> dict[str, str]:
    return {kind: str(path) for kind, path in artifacts.items()}


def _speed_payload(result: RunResult) -> dict[str, float]:
    speed_factor = (
        result.video_duration_seconds / result.elapsed_seconds
        if result.elapsed_seconds > 0 and result.video_duration_seconds > 0
        else 0.0
    )
    return {
        "elapsed_seconds": result.elapsed_seconds,
        "video_duration_seconds": result.video_duration_seconds,
        "speed_factor": speed_factor,
    }
