"""Benchmark diagnostic metrics and report tests."""

from __future__ import annotations

import csv
import json
from pathlib import Path

import pytest

from sublift.benchmark.config import RunConfig
from sublift.benchmark.diagnostics import (
    FN_BOUNDARY,
    FN_MERGED,
    FP_FALSE_ALARM,
    FP_SPLIT_EXTRA,
    OK,
    TEXT_EMPTY,
    analyze_entries,
)
from sublift.benchmark.report import write_reports
from sublift.benchmark.result import RunResult
from sublift.benchmark.srt import SrtEntry


def _gt(index: int, start: int, end: int, text: str = "text") -> SrtEntry:
    return SrtEntry(index=index, start_ms=start, end_ms=end, text=text)


def _det(index: int, start: int, end: int, text: str = "text") -> SrtEntry:
    return SrtEntry(index=index, start_ms=start, end_ms=end, text=text)


def test_diagnostics_perfect_match() -> None:
    analysis = analyze_entries([_det(1, 0, 1000, "hello")], [_gt(1, 0, 1000, "hello")])

    assert analysis.metrics.timing.timing_recall == 1.0
    assert analysis.metrics.timing.timing_precision == 1.0
    assert analysis.metrics.timing.timing_f1 == 1.0
    assert analysis.gt_cases[0].classification == OK
    assert analysis.det_cases[0].classification == OK
    assert analysis.metrics.recognition.exact_match_rate == 1.0


def test_diagnostics_classifies_detection_covering_multiple_gt_as_merge() -> None:
    analysis = analyze_entries(
        [_det(1, 0, 2000)],
        [_gt(1, 0, 1000), _gt(2, 1000, 2000)],
    )

    assert analysis.metrics.timing.timing_recall == 0.5
    assert analysis.metrics.timing.merge_count == 1
    assert [case.classification for case in analysis.gt_cases] == [OK, FN_MERGED]


def test_diagnostics_classifies_extra_detection_for_same_gt_as_split() -> None:
    analysis = analyze_entries(
        [_det(1, 0, 1000), _det(2, 1000, 2000)],
        [_gt(1, 0, 2000)],
    )

    assert analysis.metrics.timing.timing_recall == 1.0
    assert analysis.metrics.timing.timing_precision == 0.5
    assert analysis.metrics.timing.split_count == 1
    assert [case.classification for case in analysis.det_cases] == [OK, FP_SPLIT_EXTRA]


def test_diagnostics_classifies_detection_without_overlap_as_false_alarm() -> None:
    analysis = analyze_entries([_det(1, 2000, 3000)], [_gt(1, 0, 1000)])

    assert analysis.metrics.timing.false_alarm_count == 1
    assert analysis.det_cases[0].classification == FP_FALSE_ALARM
    assert analysis.gt_cases[0].classification == "timing.fn.no_overlap"


def test_diagnostics_classifies_overlap_below_iou_threshold_as_boundary_miss() -> None:
    analysis = analyze_entries([_det(1, 600, 1600)], [_gt(1, 0, 1000)])

    assert analysis.metrics.timing.miss_count == 1
    assert analysis.gt_cases[0].classification == FN_BOUNDARY
    assert analysis.gt_cases[0].temporal_iou == pytest.approx(0.25)


def test_diagnostics_macro_and_micro_cer_have_different_weights() -> None:
    analysis = analyze_entries(
        [_det(1, 0, 1000, "b"), _det(2, 2000, 3000, "aaab")],
        [_gt(1, 0, 1000, "a"), _gt(2, 2000, 3000, "aaaa")],
    )

    assert analysis.metrics.recognition.cer_macro == pytest.approx(0.625)
    assert analysis.metrics.recognition.cer_micro == pytest.approx(0.4)
    assert analysis.metrics.recognition.char_accuracy == pytest.approx(0.6)


def test_diagnostics_empty_text_counts_as_empty_text_rate() -> None:
    analysis = analyze_entries([_det(1, 0, 1000, "")], [_gt(1, 0, 1000, "hello")])

    assert analysis.gt_cases[0].classification == TEXT_EMPTY
    assert analysis.metrics.recognition.empty_text_rate == 1.0
    assert analysis.metrics.e2e.usable_subtitle_recall == 0.0


def test_diagnostics_exact_match_uses_normalized_text() -> None:
    analysis = analyze_entries([_det(1, 0, 1000, "你好")], [_gt(1, 0, 1000, "你 好")])

    assert analysis.gt_cases[0].exact_match is True
    assert analysis.metrics.recognition.exact_match_rate == 1.0


def test_agent_reports_have_stable_schema_and_files(tmp_path: Path) -> None:
    config = RunConfig(
        video_path=Path("debug/movie.mp4"),
        ground_truth_path=Path("benchmark/datasets/movie.srt"),
        label="unit",
        output_dir=tmp_path,
    )
    result = RunResult(
        config=config,
        detected=[_det(1, 0, 1000, "hello")],
        ground_truth=[_gt(1, 0, 1000, "hello")],
        elapsed_seconds=0.0,
        video_duration_seconds=0.0,
    )

    paths = write_reports(result)

    payload = json.loads(paths["agent_json"].read_text(encoding="utf-8"))
    assert set(["run", "summary", "gates", "failure_clusters", "cases", "artifacts"]).issubset(
        payload
    )
    assert payload["run"]["pipeline"] == {}

    gt_rows = list(csv.reader(paths["gt_cases_csv"].read_text(encoding="utf-8").splitlines()))
    det_rows = list(csv.reader(paths["det_cases_csv"].read_text(encoding="utf-8").splitlines()))
    assert gt_rows[0] == [
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
    assert det_rows[0] == [
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

    summary = paths["summary_markdown"].read_text(encoding="utf-8")
    assert "conclusion" in summary
    assert "main_improvement" in summary
    assert "primary_remaining_gap" in summary
