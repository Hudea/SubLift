"""feat-037a: PerformanceRecorder 单测（fake clock / 聚合 / 序列化）。"""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from sublift.diagnostics.performance import (
    PIPELINE_OVERHEAD_CHILD_STAGES,
    STAGE_OCR,
    STAGE_PIPELINE_OVERHEAD,
    PerformanceMode,
    PerformanceRecorder,
    StageStats,
    aggregate_run_payloads,
    parse_performance_mode,
)


class FakeClock:
    """可控单调时钟（纳秒）。"""

    def __init__(self, start: int = 0) -> None:
        self.now = start

    def __call__(self) -> int:
        return self.now

    def advance(self, ns: int) -> None:
        self.now += ns


def test_parse_performance_mode_accepts_valid() -> None:
    assert parse_performance_mode("off") is PerformanceMode.OFF
    assert parse_performance_mode("SUMMARY") is PerformanceMode.SUMMARY
    assert parse_performance_mode(PerformanceMode.TRACE) is PerformanceMode.TRACE


def test_parse_performance_mode_rejects_invalid() -> None:
    with pytest.raises(ValueError, match=r"performance\.mode"):
        parse_performance_mode("debug")


def test_recorder_rejects_off_mode() -> None:
    with pytest.raises(ValueError, match="off"):
        PerformanceRecorder(mode=PerformanceMode.OFF)


def test_nested_spans_and_exception_still_recorded() -> None:
    clock = FakeClock()
    rec = PerformanceRecorder(mode="summary", clock=clock)
    rec.mark_core_start()

    with rec.span("outer"):
        clock.advance(100)
        with rec.span("inner"):
            clock.advance(50)
        clock.advance(25)
        try:
            with rec.span("failing"):
                clock.advance(10)
                raise RuntimeError("boom")
        except RuntimeError:
            pass

    rec.mark_core_end()
    payload = rec.to_payload()
    assert payload["mode"] == "summary"
    assert payload["completed"] is True
    assert payload["stages"]["outer"]["count"] == 1
    assert payload["stages"]["outer"]["total_ms"] == pytest.approx(0.000185)
    assert payload["stages"]["inner"]["total_ms"] == pytest.approx(0.000050)
    assert payload["stages"]["failing"]["count"] == 1
    assert payload["stages"]["failing"]["total_ms"] == pytest.approx(0.000010)
    assert payload["throughput"]["core_wall_ms"] == pytest.approx(0.000185)


def test_counters_and_first_frame_entry_latency() -> None:
    clock = FakeClock(1_000_000_000)
    rec = PerformanceRecorder(mode="summary", clock=clock)
    rec.mark_core_start()
    rec.mark_spawn()
    clock.advance(5_000_000)  # 5 ms
    rec.mark_first_frame()
    rec.incr("frame_count", 3)
    rec.incr("raw_output_bytes", 100)
    rec.incr("raw_output_bytes", 50)
    clock.advance(20_000_000)  # +20 ms
    rec.mark_first_entry()
    rec.mark_core_end()
    rec.set_workload({"video_duration_seconds": 10.0})

    payload = rec.to_payload()
    assert payload["latency"]["spawn_to_first_frame_ms"] == pytest.approx(5.0)
    assert payload["latency"]["first_frame_ms"] == pytest.approx(5.0)
    assert payload["latency"]["first_entry_ms"] == pytest.approx(25.0)
    assert payload["counters"]["frame_count"] == 3
    assert payload["throughput"]["raw_output_bytes"] == 150
    # core wall 25ms → realtime 10 / 0.025 = 400
    assert payload["throughput"]["realtime_factor"] == pytest.approx(400.0)
    # 无阶段耗时 → unattributed ≈ core
    assert payload["throughput"]["unattributed_ms"] == pytest.approx(25.0)
    assert payload["throughput"]["stage_coverage_pct"] == pytest.approx(0.0)


def test_attribution_excludes_finalize_container() -> None:
    clock = FakeClock()
    rec = PerformanceRecorder(mode="summary", clock=clock)
    rec.mark_core_start()
    with rec.span("ocr", sample=True):
        clock.advance(10_000_000)  # 10 ms
    with rec.span("finalize"):
        clock.advance(5_000_000)  # nested container, should not double-count
        with rec.span("dedupe"):
            clock.advance(2_000_000)
    rec.mark_core_end()
    payload = rec.to_payload()
    thr = payload["throughput"]
    # leaf: ocr 10ms + dedupe 2ms = 12ms; finalize excluded
    assert thr["attributed_stage_ms"] == pytest.approx(12.0)
    assert thr["core_wall_ms"] == pytest.approx(17.0)
    assert thr["unattributed_ms"] == pytest.approx(5.0)
    assert thr["stage_coverage_pct"] == pytest.approx(12.0 / 17.0 * 100.0)


def test_exclusive_span_records_only_uncovered_pipeline_time() -> None:
    """排他 span 扣除叶子阶段，但保留 finalize 容器自身的编排时间。"""
    clock = FakeClock()
    rec = PerformanceRecorder(mode="summary", clock=clock)
    rec.mark_core_start()

    with rec.exclusive_span(
        STAGE_PIPELINE_OVERHEAD,
        child_stages=PIPELINE_OVERHEAD_CHILD_STAGES,
    ):
        clock.advance(3_000_000)  # 帧迭代 / 状态机开销
        with rec.span(STAGE_OCR, sample=True):
            clock.advance(10_000_000)
        clock.advance(2_000_000)  # OCR 后的事件编排
        with rec.span("finalize"):
            clock.advance(5_000_000)  # finalize 容器本身
            with rec.span("dedupe"):
                clock.advance(2_000_000)

    rec.mark_core_end()
    payload = rec.to_payload()
    thr = payload["throughput"]

    # pipeline overhead = 3 + 2 + 5；ocr/dedupe 仍各自独立、无重叠。
    assert payload["stages"][STAGE_PIPELINE_OVERHEAD]["total_ms"] == pytest.approx(10.0)
    assert payload["stages"][STAGE_PIPELINE_OVERHEAD]["definition"]
    assert thr["attributed_stage_ms"] == pytest.approx(22.0)
    assert thr["unattributed_ms"] == pytest.approx(0.0)
    assert thr["stage_coverage_pct"] == pytest.approx(100.0)


def test_ocr_samples_bounded_and_percentiles() -> None:
    stats = StageStats(sample_cap=4)
    for ns in (10, 20, 30, 40, 50, 60):
        stats.add(ns)
    assert len(stats.samples_ns) == 4
    assert stats.count == 6
    # 环形覆盖后 samples 为后写入槽位内容
    assert stats.percentile_ns(50.0) is not None

    clock = FakeClock()
    rec = PerformanceRecorder(mode="summary", clock=clock)
    for d in (1_000_000, 2_000_000, 3_000_000, 4_000_000, 10_000_000):
        with rec.span(STAGE_OCR, sample=True):
            clock.advance(d)
    payload = rec.to_payload()
    ocr = payload["stages"][STAGE_OCR]
    assert ocr["count"] == 5
    assert "p50_ms" in ocr
    assert "p95_ms" in ocr
    assert ocr["max_ms"] == pytest.approx(10.0)


def test_trace_writes_segment_jsonl_without_text(tmp_path: Path) -> None:
    path = tmp_path / "segments.jsonl"
    clock = FakeClock()
    rec = PerformanceRecorder(mode="trace", segment_path=path, clock=clock)
    rec.record_segment(
        start_ms=100,
        end_ms=500,
        representative_frames=3,
        ocr_calls=2,
        ocr_wall_ns=12_000_000,
        select_wall_ns=1_000_000,
        accepted=True,
        output_chars=8,
    )
    rec.note_incomplete("cancelled")
    payload = rec.to_payload()
    rec.close()

    assert payload["completed"] is False
    assert payload["incomplete_reason"] == "cancelled"
    assert payload["segment_count"] == 1
    lines = path.read_text(encoding="utf-8").strip().splitlines()
    assert len(lines) == 1
    row = json.loads(lines[0])
    assert row["start_ms"] == 100
    assert row["end_ms"] == 500
    assert row["accepted"] is True
    assert row["output_chars"] == 8
    assert "text" not in row


def test_summary_mode_ignores_segment_records(tmp_path: Path) -> None:
    path = tmp_path / "segments.jsonl"
    rec = PerformanceRecorder(mode="summary", segment_path=path)
    rec.record_segment(
        start_ms=0,
        end_ms=100,
        representative_frames=1,
        ocr_calls=1,
        ocr_wall_ns=1,
        select_wall_ns=1,
        accepted=False,
        output_chars=0,
    )
    payload = rec.to_payload()
    rec.close()
    assert "segment_count" not in payload
    assert not path.exists() or path.read_text(encoding="utf-8") == ""


def test_aggregate_run_payloads_median() -> None:
    runs = [
        {
            "throughput": {
                "core_wall_ms": 10.0,
                "realtime_factor": 20.0,
                "raw_output_bytes": 100,
                "ocr_calls": 1,
            },
            "latency": {
                "first_frame_ms": 1.0,
                "first_entry_ms": 2.0,
                "spawn_to_first_frame_ms": 1.0,
            },
            "resources": {"python_peak_rss_bytes": 1000},
            "stages": {"ocr": {"total_ms": 5.0}},
        },
        {
            "throughput": {
                "core_wall_ms": 30.0,
                "realtime_factor": 10.0,
                "raw_output_bytes": 300,
                "ocr_calls": 3,
            },
            "latency": {
                "first_frame_ms": 3.0,
                "first_entry_ms": 6.0,
                "spawn_to_first_frame_ms": 3.0,
            },
            "resources": {"python_peak_rss_bytes": 3000},
            "stages": {"ocr": {"total_ms": 15.0}},
        },
        {
            "throughput": {
                "core_wall_ms": 20.0,
                "realtime_factor": 15.0,
                "raw_output_bytes": 200,
                "ocr_calls": 2,
            },
            "latency": {
                "first_frame_ms": 2.0,
                "first_entry_ms": 4.0,
                "spawn_to_first_frame_ms": 2.0,
            },
            "resources": {"python_peak_rss_bytes": 2000},
            "stages": {"ocr": {"total_ms": 10.0}},
        },
    ]
    agg = aggregate_run_payloads(runs)
    assert agg["measured_runs"] == 3
    assert agg["core_wall_ms"]["median"] == pytest.approx(20.0)
    assert agg["core_wall_ms"]["min"] == pytest.approx(10.0)
    assert agg["core_wall_ms"]["max"] == pytest.approx(30.0)
    assert agg["stage_total_ms"]["ocr"]["median"] == pytest.approx(10.0)
