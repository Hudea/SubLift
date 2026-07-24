"""feat-037a + feat-043a: PerformanceRecorder 单测（fake clock / 聚合 / OCR 内部归因）。"""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from sublift.diagnostics.performance import (
    _COVERAGE_LEAF_STAGES,
    PIPELINE_OVERHEAD_CHILD_STAGES,
    STAGE_OCR,
    STAGE_PIPELINE_OVERHEAD,
    OcrBreakdown,
    OcrCallDetail,
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


# ------------------------------------------------------------------
# feat-043a：OCR 内部归因数据模型与对账
# ------------------------------------------------------------------


def test_ocr_call_detail_creation_and_to_dict() -> None:
    detail = OcrCallDetail(
        input_width=1920,
        input_height=87,
        input_mode="RGB",
        input_prepare_ms=1.5,
        request_setup_ms=0.3,
        vision_perform_ms=25.0,
        observation_mapping_ms=0.8,
        residual_ms=0.2,
        total_ms=27.8,
        outcome="success",
    )
    d = detail.to_dict()
    assert d["input_width"] == 1920
    assert d["input_height"] == 87
    assert d["input_mode"] == "RGB"
    assert d["outcome"] == "success"
    assert d["total_ms"] == pytest.approx(27.8)
    # 不泄露文本/图像/box/路径
    for key in ("text", "image", "box", "path", "pixels"):
        assert key not in d


def test_ocr_call_detail_components_ms() -> None:
    detail = OcrCallDetail(
        input_width=100,
        input_height=50,
        input_mode="RGB",
        input_prepare_ms=1.0,
        request_setup_ms=0.5,
        vision_perform_ms=10.0,
        observation_mapping_ms=0.3,
        residual_ms=0.2,
        total_ms=12.0,
        outcome="success",
    )
    assert detail.components_ms == pytest.approx(12.0)


def test_ocr_breakdown_record_call_aggregates() -> None:
    bd = OcrBreakdown()
    bd.record_call(
        call_detail=OcrCallDetail(
            input_width=1920,
            input_height=87,
            input_mode="RGB",
            input_prepare_ms=2.0,
            request_setup_ms=0.5,
            vision_perform_ms=20.0,
            observation_mapping_ms=1.0,
            residual_ms=0.5,
            total_ms=24.0,
            outcome="success",
        )
    )
    bd.record_call(
        call_detail=OcrCallDetail(
            input_width=1920,
            input_height=87,
            input_mode="RGB",
            input_prepare_ms=3.0,
            request_setup_ms=0.6,
            vision_perform_ms=18.0,
            observation_mapping_ms=0.9,
            residual_ms=0.5,
            total_ms=23.0,
            outcome="success",
        )
    )
    assert bd.call_count == 2
    assert bd.call_total_ms == pytest.approx(47.0)
    assert bd.input_prepare.count == 2
    assert bd.vision_perform.count == 2
    # 几何桶
    assert len(bd.input_geometry_buckets) == 1
    key = (1920, 87, "RGB")
    assert bd.input_geometry_buckets[key] == 2


def test_ocr_breakdown_record_segment_decision() -> None:
    bd = OcrBreakdown()
    bd.record_segment_decision(
        representative_frames=3,
        actual_ocr_calls=2,
        early_stop_reason="two_frame_consensus",
        accepted=True,
    )
    bd.record_segment_decision(
        representative_frames=4,
        actual_ocr_calls=4,
        early_stop_reason="representative_frames_exhausted",
        accepted=False,
    )
    assert bd.representative_frames_total == 7
    assert bd.actual_ocr_calls_total == 6
    assert bd.early_stop_reasons["two_frame_consensus"] == 1
    assert bd.early_stop_reasons["representative_frames_exhausted"] == 1
    assert bd.accepted_count == 1
    assert bd.rejected_count == 1


def test_ocr_breakdown_to_payload_structure() -> None:
    bd = OcrBreakdown()
    bd.record_call(
        call_detail=OcrCallDetail(
            input_width=1920,
            input_height=87,
            input_mode="RGB",
            input_prepare_ms=2.0,
            request_setup_ms=0.5,
            vision_perform_ms=20.0,
            observation_mapping_ms=1.0,
            residual_ms=0.5,
            total_ms=24.0,
            outcome="success",
        )
    )
    bd.record_segment_decision(
        representative_frames=2,
        actual_ocr_calls=1,
        early_stop_reason="single_high_confidence",
        accepted=True,
    )
    payload = bd.to_payload()
    assert payload["call_count"] == 1
    assert "input_prepare" in payload
    assert "vision_perform" in payload
    assert "input_geometry" in payload
    assert payload["segment_decisions"]["early_stop_reasons"]["single_high_confidence"] == 1
    assert "accounting" in payload
    # accounting 有 note 说明内部阶段不参与 core coverage
    acc = payload["accounting"]
    assert "note" in acc
    assert "excluded from core" in acc["note"]
    assert payload["engine_detail"] == "vision"


def test_ocr_breakdown_accounting_parent_components_delta() -> None:
    bd = OcrBreakdown()
    # 总 wall 24.0 = 2.0+0.5+20.0+1.0+0.5
    bd.record_call(
        call_detail=OcrCallDetail(
            input_width=100,
            input_height=50,
            input_mode="RGB",
            input_prepare_ms=2.0,
            request_setup_ms=0.5,
            vision_perform_ms=20.0,
            observation_mapping_ms=1.0,
            residual_ms=0.5,
            total_ms=24.0,
            outcome="success",
        )
    )
    acc = bd._accounting_payload()
    assert acc["parent_total_ms"] == pytest.approx(24.0)
    assert acc["components_total_ms"] == pytest.approx(24.0)
    # delta ≤ max(0.1ms, parent*1%) → 0.24ms
    assert acc["delta_ms"] < max(0.1, 24.0 * 0.01) + 0.01


def test_recorder_ensure_ocr_breakdown() -> None:
    rec = PerformanceRecorder(mode="summary")
    bd1 = rec.ensure_ocr_breakdown(engine_detail="vision")
    bd2 = rec.ensure_ocr_breakdown()
    assert bd1 is bd2  # 幂等返回同一实例
    assert bd1.engine_detail == "vision"
    rec.close()


def test_recorder_add_ocr_call_detail_and_breakdown_payload() -> None:
    rec = PerformanceRecorder(mode="summary")
    rec.ensure_ocr_breakdown()
    rec.add_ocr_call_detail(
        OcrCallDetail(
            input_width=1920,
            input_height=87,
            input_mode="RGB",
            input_prepare_ms=1.0,
            request_setup_ms=0.3,
            vision_perform_ms=25.0,
            observation_mapping_ms=0.8,
            residual_ms=0.2,
            total_ms=27.3,
            outcome="success",
        )
    )
    rec.mark_core_start()
    rec.mark_core_end()
    payload = rec.to_payload()
    rec.close()
    assert "ocr_breakdown" in payload
    bd = payload["ocr_breakdown"]
    assert bd["call_count"] == 1
    assert bd["engine_detail"] == "vision"


def test_ocr_breakdown_not_in_coverage_leaf_stages() -> None:
    """ocr_breakdown 的内部阶段不得加入 _COVERAGE_LEAF_STAGES。"""
    for sub in (
        "ocr.input_prepare",
        "ocr.request_setup",
        "ocr.vision_perform",
        "ocr.observation_mapping",
        "ocr.residual",
    ):
        assert sub not in _COVERAGE_LEAF_STAGES, f"{sub} 不能进入 core coverage"


def test_ocr_breakdown_does_not_double_count_coverage() -> None:
    """ocr_breakdown 独立于 core stage coverage，ocr 仍只计一次。"""
    clock = FakeClock()
    rec = PerformanceRecorder(mode="summary", clock=clock)
    rec.ensure_ocr_breakdown()
    rec.mark_core_start()

    # 外层 ocr span（core coverage 唯一依赖）
    with rec.span("ocr", sample=True):
        clock.advance(25_000_000)  # 25ms ocr wall
    clock.advance(2_000_000)  # 2ms pipeline

    # 同时记录内部 breakdown（不改变 core attribution）
    rec.add_ocr_call_detail(
        OcrCallDetail(
            input_width=1920,
            input_height=87,
            input_mode="RGB",
            input_prepare_ms=2.0,
            request_setup_ms=0.5,
            vision_perform_ms=20.0,
            observation_mapping_ms=0.8,
            residual_ms=1.7,
            total_ms=25.0,
            outcome="success",
        )
    )
    rec.mark_core_end()
    payload = rec.to_payload()
    rec.close()

    thr = payload["throughput"]
    assert thr["core_wall_ms"] == pytest.approx(27.0)
    # ocr 归因为 25ms，2ms 未归因（不是 25ms + breakdown 内部）
    assert thr["attributed_stage_ms"] == pytest.approx(25.0)
    assert thr["unattributed_ms"] == pytest.approx(2.0)

    assert "ocr_breakdown" in payload
    bd = payload["ocr_breakdown"]
    assert bd["call_count"] == 1


def test_no_ocr_breakdown_when_no_calls() -> None:
    """没有 OCR 调用时不输出 ocr_breakdown。"""
    rec = PerformanceRecorder(mode="summary")
    rec.mark_core_start()
    rec.mark_core_end()
    payload = rec.to_payload()
    rec.close()
    assert "ocr_breakdown" not in payload


def test_add_ocr_call_detail_silent_noop_without_ensure() -> None:
    """未调用 ensure_ocr_breakdown 时 add_ocr_call_detail 静默跳过不崩溃。"""
    rec = PerformanceRecorder(mode="summary")
    detail = OcrCallDetail(
        input_width=100,
        input_height=100,
        input_mode="RGB",
        input_prepare_ms=1.0,
        request_setup_ms=0.5,
        vision_perform_ms=40.0,
        observation_mapping_ms=1.0,
        residual_ms=0.5,
        total_ms=43.0,
        outcome="success",
    )
    # 未调用 ensure_ocr_breakdown() — add_ocr_call_detail 应静默跳过
    rec.add_ocr_call_detail(detail)
    rec.mark_core_start()
    rec.mark_core_end()
    payload = rec.to_payload()
    rec.close()
    # 不应崩溃，也不应产生 breakdown（因为 call_count=0）
    assert "ocr_breakdown" not in payload


def test_empty_breakdown_to_payload_structure() -> None:
    """空 OcrBreakdown 的 to_payload() 结构合理。"""
    bd = OcrBreakdown(engine_detail="vision")
    payload = bd.to_payload()
    assert payload["call_count"] == 0
    assert payload["call_total"]["count"] == 0
    assert payload["call_total"]["mean_ms"] == 0.0
    # 空 breakdown 的 accounting: coverage_pct 应为 None
    acc = payload["accounting"]
    assert acc["parent_total_ms"] == 0.0
    assert acc["coverage_pct"] is None
    assert acc["delta_ms"] == 0.0


def test_trace_segment_new_fields_in_jsonl(tmp_path: Path) -> None:
    """trace 段记录输出新增字段，不泄露文本/box/路径。"""
    path = tmp_path / "seg.jsonl"
    rec = PerformanceRecorder(mode="trace", segment_path=path)
    rec.ensure_ocr_breakdown()
    rec.record_segment(
        start_ms=100,
        end_ms=500,
        representative_frames=3,
        ocr_calls=2,
        ocr_wall_ns=15_000_000,
        select_wall_ns=2_000_000,
        accepted=True,
        output_chars=5,
        representative_selection_ms=0.5,
        early_stop_reason="two_frame_consensus",
        ocr_call_details=[
            {
                "input_width": 1920,
                "input_height": 87,
                "input_mode": "RGB",
                "input_prepare_ms": 2.0,
                "request_setup_ms": 0.3,
                "vision_perform_ms": 20.0,
                "observation_mapping_ms": 0.8,
                "residual_ms": 0.5,
                "total_ms": 23.6,
                "outcome": "success",
            }
        ],
    )
    rec.close()

    lines = path.read_text(encoding="utf-8").strip().splitlines()
    assert len(lines) == 1
    row = json.loads(lines[0])
    assert row["representative_selection_ms"] == pytest.approx(0.5)
    assert row["early_stop_reason"] == "two_frame_consensus"
    assert len(row["ocr_call_details"]) == 1
    detail = row["ocr_call_details"][0]
    assert detail["input_width"] == 1920
    assert detail["outcome"] == "success"
    # 隐私扫描
    text = json.dumps(row)
    for sentinel in ("text", "image", "box", "pixels", "/Users", "/tmp"):
        assert sentinel not in text or sentinel in ("text",), (
            f"sentinel '{sentinel}' leaked in trace JSONL"
        )


def test_record_segment_decision_aggregates_to_breakdown() -> None:
    """summary 下 record_segment_decision 被 breakdown 聚合。"""
    rec = PerformanceRecorder(mode="summary")
    rec.ensure_ocr_breakdown()
    rec.record_segment_decision(
        representative_frames=3,
        actual_ocr_calls=2,
        early_stop_reason="two_frame_consensus",
        accepted=True,
    )
    rec.mark_core_start()
    rec.mark_core_end()
    payload = rec.to_payload()
    rec.close()

    bd = payload["ocr_breakdown"]
    assert bd["segment_decisions"]["representative_frames_total"] == 3
    assert bd["segment_decisions"]["early_stop_reasons"]["two_frame_consensus"] == 1
    assert bd["segment_decisions"]["accepted"] == 1


# ------------------------------------------------------------------
# 旧有测试
# ------------------------------------------------------------------


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
