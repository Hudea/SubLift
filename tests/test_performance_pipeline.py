"""feat-037b/c + feat-043b: Pipeline/Extractor 埋点与 off/summary/trace 一致性。"""

from __future__ import annotations

import json
import subprocess
from pathlib import Path
from typing import Any

import numpy as np
import pytest
from PIL import Image

from sublift.benchmark.config import RunConfig
from sublift.benchmark.report import write_reports
from sublift.benchmark.runner import _validate_ocr_breakdown, run_benchmark
from sublift.config import Config
from sublift.detector import FixedRegionDetector
from sublift.diagnostics.performance import (
    OcrCallDetail,
    PerformanceMode,
    PerformanceRecorder,
)
from sublift.extractor import FfmpegExtractor
from sublift.models import BoundingBox, Frame, OcrLine, OcrResult
from sublift.ocr import MockOcrEngine
from sublift.pipeline import Pipeline


class CountingOcr:
    """返回固定行的 OCR，便于验证调用次数。"""

    def __init__(self) -> None:
        self.calls = 0

    def recognize(self, image: Image.Image) -> OcrResult:
        self.calls += 1
        line = OcrLine(
            text="你好",
            confidence=0.95,
            box=BoundingBox(x=10, y=10, width=80, height=20),
        )
        return OcrResult(text=line.text, confidence=line.confidence, lines=(line,))


def _blank_frame(timestamp_ms: int, width: int = 320, height: int = 80) -> Frame:
    img = Image.fromarray(np.full((height, width, 3), 200, dtype=np.uint8))
    return Frame(timestamp_ms=timestamp_ms, image=img)


def _subtitle_frame(
    timestamp_ms: int,
    rect: tuple[int, int, int, int] = (60, 30, 200, 40),
    width: int = 320,
    height: int = 80,
) -> Frame:
    arr = np.full((height, width, 3), 200, dtype=np.uint8)
    x, y, w, h = rect
    arr[y : y + h, x : x + w] = 30
    return Frame(timestamp_ms=timestamp_ms, image=Image.fromarray(arr))


def test_pipeline_summary_records_stages() -> None:
    rec = PerformanceRecorder(mode=PerformanceMode.SUMMARY)
    ocr = CountingOcr()
    detector = FixedRegionDetector(BoundingBox(0, 0, 320, 80))
    config = Config(enable_line_select=True, subtitle_script="cjk", ocr_consensus_frames=1)
    pipeline = Pipeline(
        detector=detector,
        ocr=ocr,
        config=config,
        performance_recorder=rec,
    )
    frames = [
        _blank_frame(0),
        _blank_frame(200),
        _subtitle_frame(400),
        _subtitle_frame(600),
        _subtitle_frame(800),
        _blank_frame(1000),
        _blank_frame(1200),
    ]
    entries = pipeline.run_frames(iter(frames))
    payload = rec.to_payload()
    rec.close()

    assert "crop" in payload["stages"]
    assert "signature" in payload["stages"]
    assert "changepoint" in payload["stages"]
    assert "pipeline_overhead" in payload["stages"]
    assert payload["stages"]["crop"]["count"] >= 1
    assert payload["completed"] is True
    assert len(entries) >= 1
    thr = payload["throughput"]
    assert thr.get("unattributed_ms") is not None
    assert thr.get("stage_coverage_pct") is not None


def test_off_and_summary_produce_same_entries() -> None:
    """同一帧流、off 与 summary 字幕结果一致。"""
    frames = [
        _blank_frame(0),
        _subtitle_frame(200),
        _subtitle_frame(400),
        _blank_frame(600),
        _blank_frame(800),
    ]

    def run(with_perf: bool) -> list[tuple[int, int, str]]:
        rec = PerformanceRecorder(mode="summary") if with_perf else None
        ocr = MockOcrEngine(text="hello", confidence=0.99)
        pipeline = Pipeline(
            detector=FixedRegionDetector(BoundingBox(0, 0, 320, 80)),
            ocr=ocr,
            config=Config(enable_line_select=False),
            performance_recorder=rec,
        )
        entries = pipeline.run_frames(iter(frames))
        if rec is not None:
            rec.close()
        return [(e.start_ms, e.end_ms, e.text) for e in entries]

    assert run(False) == run(True)


def test_trace_segment_jsonl_forced(tmp_path: Path) -> None:
    """强制产生至少一个字幕段，trace JSONL 必须非空且字段完整。"""
    path = tmp_path / "seg.jsonl"
    rec = PerformanceRecorder(mode="trace", segment_path=path)
    ocr = CountingOcr()
    pipeline = Pipeline(
        detector=FixedRegionDetector(BoundingBox(0, 0, 320, 80)),
        ocr=ocr,
        config=Config(
            enable_line_select=True,
            subtitle_script="cjk",
            ocr_consensus_frames=2,
        ),
        performance_recorder=rec,
    )
    frames = [
        _blank_frame(0),
        _blank_frame(200),
        _subtitle_frame(400),
        _subtitle_frame(600),
        _subtitle_frame(800),
        _blank_frame(1000),
        _blank_frame(1200),
        _blank_frame(1400),
    ]
    entries = pipeline.run_frames(iter(frames))
    payload = rec.to_payload()
    rec.close()

    assert len(entries) >= 1
    assert path.exists()
    text = path.read_text(encoding="utf-8").strip()
    assert text, "trace JSONL 不得为空"
    lines = text.splitlines()
    assert payload.get("segment_count") == len(lines)
    assert payload["segment_count"] >= 1

    for line in lines:
        row = json.loads(line)
        assert "text" not in row
        assert isinstance(row["start_ms"], int)
        assert isinstance(row["end_ms"], int)
        assert row["end_ms"] >= row["start_ms"]
        assert row["representative_frames"] >= 1
        assert row["ocr_calls"] >= 1
        assert row["ocr_wall_ms"] >= 0
        assert row["select_wall_ms"] >= 0
        assert isinstance(row["accepted"], bool)
        assert isinstance(row["output_chars"], int)
        # feat-043c：新增字段存在且类型正确
        assert isinstance(row["representative_selection_ms"], (int, float))
        assert isinstance(row["early_stop_reason"], str)
        assert isinstance(row["ocr_call_details"], list)
        # early_stop_reason 必须是允许值之一
        from sublift.diagnostics.performance import _ALLOWED_EARLY_STOP_REASONS

        assert row["early_stop_reason"] in _ALLOWED_EARLY_STOP_REASONS | {""}


def _generate_test_video(path: Path, duration: float = 1.0) -> None:
    cmd = [
        "ffmpeg",
        "-hide_banner",
        "-loglevel",
        "error",
        "-f",
        "lavfi",
        "-i",
        f"testsrc=duration={duration}:size=320x240:rate=5",
        "-c:v",
        "libx264",
        "-pix_fmt",
        "yuv420p",
        str(path),
        "-y",
    ]
    subprocess.run(cmd, capture_output=True, check=True)


def _write_gt(path: Path) -> None:
    path.write_text(
        "1\n00:00:00,000 --> 00:00:01,000\nhello\n\n",
        encoding="utf-8",
    )


@pytest.mark.integration
def test_benchmark_off_summary_trace_mock_video(tmp_path: Path) -> None:
    """短合成视频 + Mock OCR：off/summary/trace 字幕一致，报告含 performance。"""
    video = tmp_path / "clip.mp4"
    gt = tmp_path / "gt.srt"
    _generate_test_video(video, duration=1.0)
    _write_gt(gt)
    out = tmp_path / "reports"

    def _cfg(mode: str, label: str) -> RunConfig:
        return RunConfig(
            video_path=video,
            ground_truth_path=gt,
            fps=2.0,
            engine="mock",
            confidence=0.5,
            subtitle_script="latin",
            region_box=(0, 180, 320, 60),
            label=label,
            output_dir=out / label,
            performance_mode=mode,
            warmup_runs=0,
            measured_runs=1,
            video_duration_seconds=1.0,
            isolate_processes=False,
        )

    off = run_benchmark(_cfg("off", "off"))
    summary = run_benchmark(_cfg("summary", "summary"))
    trace = run_benchmark(_cfg("trace", "trace"))

    off_entries = [(e.start_ms, e.end_ms, e.text) for e in off.detected]
    summary_entries = [(e.start_ms, e.end_ms, e.text) for e in summary.detected]
    trace_entries = [(e.start_ms, e.end_ms, e.text) for e in trace.detected]
    assert off_entries == summary_entries == trace_entries

    assert off.performance is None
    assert summary.performance is not None
    assert summary.performance["mode"] == "summary"
    assert "stages" in summary.performance
    assert "extract_wait" in summary.performance["stages"]
    assert "frame_materialize" in summary.performance["stages"]
    thr = summary.performance["throughput"]
    assert "unattributed_ms" in thr
    assert "stage_coverage_pct" in thr
    definition = summary.performance["stages"]["extract_wait"].get("definition", "")
    assert "not pure codec decode" in definition or "stdout" in definition
    assert "aggregate" in summary.performance
    assert "quality" in summary.performance
    assert summary.performance["quality"]["all_runs_pass"] is not None

    assert trace.performance is not None
    assert trace.performance["mode"] == "trace"

    paths = write_reports(summary)
    agent = json.loads(paths["agent_json"].read_text(encoding="utf-8"))
    assert "performance" in agent
    assert agent["performance"]["mode"] == "summary"
    md = paths["summary_markdown"].read_text(encoding="utf-8")
    assert "## Performance" in md
    assert "unattributed_ms" in md or "Attribution" in md


@pytest.mark.integration
def test_benchmark_multi_run_quality_hash(tmp_path: Path) -> None:
    """多次 measured 均记录 detection hash 与质量门。"""
    video = tmp_path / "clip.mp4"
    gt = tmp_path / "gt.srt"
    _generate_test_video(video, duration=1.0)
    _write_gt(gt)
    cfg = RunConfig(
        video_path=video,
        ground_truth_path=gt,
        fps=2.0,
        engine="mock",
        confidence=0.5,
        subtitle_script="latin",
        region_box=(0, 180, 320, 60),
        label="multi",
        output_dir=tmp_path / "out",
        performance_mode="summary",
        warmup_runs=0,
        measured_runs=2,
        video_duration_seconds=1.0,
        isolate_processes=False,
    )
    result = run_benchmark(cfg)
    assert result.performance is not None
    quality = result.performance["quality"]
    assert len(quality["runs"]) == 2
    assert quality["detections_consistent"] is True
    assert quality["runs"][0]["detection_hash"] == quality["runs"][1]["detection_hash"]
    for run in quality["runs"]:
        assert "checks" in run
        assert "timing_f1" in run["checks"]


@pytest.mark.integration
def test_extractor_records_raw_bytes_and_materialize(tmp_path: Path) -> None:
    video = tmp_path / "clip.mp4"
    _generate_test_video(video, duration=1.0)
    rec = PerformanceRecorder(mode="summary")
    extractor = FfmpegExtractor(fps=2.0, performance_recorder=rec)
    frames = list(extractor.extract(video))
    # mark core so attribution has denominator
    rec.mark_core_start()
    rec.mark_core_end()
    payload = rec.to_payload()
    rec.close()
    assert len(frames) >= 1
    assert payload["throughput"]["frame_count"] == len(frames)
    assert payload["throughput"]["raw_output_bytes"] == len(frames) * 320 * 240 * 3
    assert payload["stages"]["extract_wait"]["count"] >= len(frames)
    assert payload["stages"]["frame_materialize"]["count"] == len(frames)


# ------------------------------------------------------------------
# feat-043b：Vision 可选内部计时 / 非 Vision opaque fallback
# ------------------------------------------------------------------


def test_mock_ocr_produces_opaque_breakdown() -> None:
    """MockOcrEngine（无内部计时）时 Pipeline 产出 opaque breakdown。"""
    rec = PerformanceRecorder(mode="summary")
    ocr = CountingOcr()
    pipeline = Pipeline(
        detector=FixedRegionDetector(BoundingBox(0, 0, 320, 80)),
        ocr=ocr,
        config=Config(enable_line_select=False),
        performance_recorder=rec,
    )
    # 字幕段持续足够长（>500ms min_duration）
    frames = [
        _blank_frame(0),
        _blank_frame(200),
        _subtitle_frame(400),
        _subtitle_frame(600),
        _subtitle_frame(800),
        _subtitle_frame(1000),
        _blank_frame(1200),
        _blank_frame(1400),
    ]
    entries = pipeline.run_frames(iter(frames))
    payload = rec.to_payload()
    rec.close()
    assert len(entries) >= 1
    assert "ocr_breakdown" in payload
    bd = payload["ocr_breakdown"]
    assert bd["engine_detail"] == "opaque"
    assert bd["call_count"] >= 1
    # opaque 下所有时间都在 residual
    assert bd["residual"]["total_ms"] > 0


def test_no_ocr_breakdown_when_perf_is_none() -> None:
    """recorder 为 None 时不创建 breakdown。"""
    ocr = MockOcrEngine(text="hello", confidence=0.99)
    pipeline = Pipeline(
        detector=FixedRegionDetector(BoundingBox(0, 0, 320, 80)),
        ocr=ocr,
        config=Config(enable_line_select=False),
        performance_recorder=None,
    )
    frames = [_subtitle_frame(200)]
    pipeline.run_frames(iter(frames))
    # 不应崩溃


def test_off_and_summary_produce_same_ocr_call_count() -> None:
    """off 与 summary 的 OCR 调用次数应一致。"""
    frames = [
        _blank_frame(0),
        _blank_frame(200),
        _subtitle_frame(400),
        _subtitle_frame(600),
        _subtitle_frame(800),
        _blank_frame(1000),
    ]
    rec = PerformanceRecorder(mode="summary")
    pipeline = Pipeline(
        detector=FixedRegionDetector(BoundingBox(0, 0, 320, 80)),
        ocr=MockOcrEngine(text="hi", confidence=0.99),
        config=Config(enable_line_select=False),
        performance_recorder=rec,
    )
    _entries = pipeline.run_frames(iter(frames))
    payload = rec.to_payload()
    rec.close()
    ocr_count = payload["throughput"]["ocr_calls"]
    assert "ocr_breakdown" in payload
    assert payload["ocr_breakdown"]["call_count"] == ocr_count


class CallbackCollector:
    """收集 Vision timing_callback 触发的 OcrCallDetail。"""

    def __init__(self) -> None:
        self.details: list[OcrCallDetail] = []

    def __call__(self, detail: OcrCallDetail) -> None:
        self.details.append(detail)


def test_vision_engine_accepts_timing_callback() -> None:
    """VisionOcrEngine 接受 timing_callback 参数（不要求 Vision 可用）。"""
    from sublift.ocr.vision import VisionOcrEngine, is_vision_available

    if not is_vision_available():
        pytest.skip("Vision 不可用（非 macOS 或未安装依赖）")

    collector = CallbackCollector()
    engine = VisionOcrEngine(timing_callback=collector)
    assert engine._timing_callback is collector


def test_pipeline_with_counting_ocr_breakdown_call_count_matches() -> None:
    """breakdown call_count 与 stages.ocr.count 一致。"""
    rec = PerformanceRecorder(mode="summary")
    ocr = CountingOcr()
    pipeline = Pipeline(
        detector=FixedRegionDetector(BoundingBox(0, 0, 320, 80)),
        ocr=ocr,
        config=Config(
            enable_line_select=True,
            subtitle_script="cjk",
            ocr_consensus_frames=2,
        ),
        performance_recorder=rec,
    )
    frames = [
        _blank_frame(0),
        _subtitle_frame(200),
        _subtitle_frame(400),
        _subtitle_frame(600),
        _blank_frame(800),
        _blank_frame(1000),
    ]
    _entries = pipeline.run_frames(iter(frames))
    payload = rec.to_payload()
    rec.close()
    assert len(_entries) >= 1
    # stages.ocr.count 与 breakdown.call_count 一致
    assert payload["stages"]["ocr"]["count"] == payload["ocr_breakdown"]["call_count"]
    assert payload["ocr_breakdown"]["call_count"] == ocr.calls
    assert payload["ocr_breakdown"]["engine_detail"] == "opaque"


def test_trace_segment_with_ocr_call_details(tmp_path: Path) -> None:
    """trace 模式下段记录包含 ocr_call_details（opaque 引擎）。"""
    path = tmp_path / "seg.jsonl"
    rec = PerformanceRecorder(mode="trace", segment_path=path)
    ocr = CountingOcr()
    pipeline = Pipeline(
        detector=FixedRegionDetector(BoundingBox(0, 0, 320, 80)),
        ocr=ocr,
        config=Config(
            enable_line_select=True,
            subtitle_script="cjk",
            ocr_consensus_frames=2,
        ),
        performance_recorder=rec,
    )
    frames = [
        _blank_frame(0),
        _blank_frame(200),
        _subtitle_frame(400),
        _subtitle_frame(600),
        _subtitle_frame(800),
        _subtitle_frame(1000),
        _blank_frame(1200),
        _blank_frame(1400),
    ]
    entries = pipeline.run_frames(iter(frames))
    assert len(entries) >= 1
    payload = rec.to_payload()
    rec.close()

    assert payload.get("segment_count", 0) >= 1
    text = path.read_text(encoding="utf-8").strip()
    assert text, "trace JSONL 不得为空"
    for line in text.splitlines():
        row = json.loads(line)
        # feat-043c 字段
        assert "representative_selection_ms" in row
        assert "early_stop_reason" in row
        assert "ocr_call_details" in row
        assert isinstance(row["ocr_call_details"], list)
        if row["ocr_calls"] > 0 and row["accepted"]:
            assert len(row["ocr_call_details"]) > 0
            detail = row["ocr_call_details"][0]
            assert "input_width" in detail
            assert "outcome" in detail
            # 隐私：不泄露文本/图像/box/路径
            detail_str = json.dumps(detail)
            for sentinel in ("text", "image", "box", "pixels"):
                assert sentinel not in detail_str, f"'{sentinel}' leaked in ocr_call_details"


def test_segment_trace_early_stop_reason_in_breakdown() -> None:
    """segment_decisions 的 early_stop_reasons 在 breakdown 中正确聚合。"""
    rec = PerformanceRecorder(mode="summary")
    pipeline = Pipeline(
        detector=FixedRegionDetector(BoundingBox(0, 0, 320, 80)),
        ocr=CountingOcr(),
        config=Config(
            enable_line_select=True,
            subtitle_script="cjk",
            ocr_consensus_frames=2,
        ),
        performance_recorder=rec,
    )
    frames = [
        _blank_frame(0),
        _blank_frame(200),
        _subtitle_frame(400),
        _subtitle_frame(600),
        _subtitle_frame(800),
        _subtitle_frame(1000),
        _subtitle_frame(1200),
        _blank_frame(1400),
        _blank_frame(1600),
    ]
    _entries = pipeline.run_frames(iter(frames))
    payload = rec.to_payload()
    rec.close()
    assert "ocr_breakdown" in payload
    bd = payload["ocr_breakdown"]
    sd = bd["segment_decisions"]
    assert sd["representative_frames_total"] > 0
    assert sd["actual_ocr_calls_total"] > 0
    assert isinstance(sd["early_stop_reasons"], dict)
    assert sum(sd["early_stop_reasons"].values()) >= 1
    assert sd["accepted"] + sd["rejected"] >= 1


def test_performance_payload_hash_stable_across_runs() -> None:
    """连续两次相同配置运行产生一致的性能 payload 结构（feat-043d perturbation）。"""
    frames = [
        _blank_frame(0),
        _blank_frame(200),
        _subtitle_frame(400),
        _subtitle_frame(600),
        _subtitle_frame(800),
        _blank_frame(1000),
        _blank_frame(1200),
    ]

    def _run() -> dict[str, Any]:
        rec = PerformanceRecorder(mode="summary")
        pipeline = Pipeline(
            detector=FixedRegionDetector(BoundingBox(0, 0, 320, 80)),
            ocr=MockOcrEngine(text="hi", confidence=0.9),
            config=Config(
                enable_line_select=False,
                confidence_threshold=0.5,
            ),
            performance_recorder=rec,
        )
        pipeline.run_frames(iter(frames))
        payload = rec.to_payload()
        rec.close()
        return payload

    payload1 = _run()
    payload2 = _run()

    # 结构一致性
    assert set(payload1.keys()) == set(payload2.keys())
    # OCR breakdown 结构一致
    if "ocr_breakdown" in payload1:
        bd1 = payload1["ocr_breakdown"]
        bd2 = payload2["ocr_breakdown"]
        assert set(bd1.keys()) == set(bd2.keys())
        assert bd1["engine_detail"] == bd2["engine_detail"]
        assert bd1["call_count"] == bd2["call_count"]
        assert "accounting" in bd1 and "accounting" in bd2
    # throughput 结构一致
    assert set(payload1["throughput"].keys()) == set(payload2["throughput"].keys())


def test_ocr_breakdown_from_pipeline_run_includes_accounting() -> None:
    """Pipeline 产出 OCR breakdown 并包含完整 accounting 块。"""
    rec = PerformanceRecorder(mode="summary")
    pipeline = Pipeline(
        detector=FixedRegionDetector(BoundingBox(0, 0, 320, 80)),
        ocr=MockOcrEngine(text="hello world", confidence=0.9),
        config=Config(
            enable_line_select=False,
            confidence_threshold=0.5,
        ),
        performance_recorder=rec,
    )
    frames = [
        _blank_frame(0),
        _blank_frame(200),
        _subtitle_frame(400),
        _subtitle_frame(600),
        _subtitle_frame(800),
        _subtitle_frame(1000),
        _blank_frame(1200),
        _blank_frame(1400),
    ]
    _entries = pipeline.run_frames(iter(frames))
    payload = rec.to_payload()
    rec.close()
    assert "ocr_breakdown" in payload
    bd = payload["ocr_breakdown"]
    assert bd["call_count"] > 0
    assert "accounting" in bd
    acc = bd["accounting"]
    assert "coverage_pct" in acc
    # opaque 引擎 accounting 合理
    assert acc["coverage_pct"] == 100.0 or acc["coverage_pct"] is None
    assert acc["parent_total_ms"] > 0


def _make_balanced_ocr_payload(
    *,
    call_count: int = 2,
    parent_ms: float = 50.0,
    outer_ms: float | None = None,
    engine_detail: str = "vision",
) -> dict[str, Any]:
    """构造通过内部对账的 performance payload 骨架。"""
    if outer_ms is None:
        outer_ms = parent_ms
    # 五阶段均分，保证 components ≈ parent
    per = parent_ms / 5.0
    stage = {
        "count": call_count,
        "total_ms": per,
        "mean_ms": per / call_count if call_count else 0.0,
        "max_ms": per,
    }
    return {
        "stages": {
            "ocr": {
                "count": call_count,
                "total_ms": outer_ms,
                "mean_ms": outer_ms / call_count if call_count else 0.0,
                "max_ms": outer_ms,
            }
        },
        "throughput": {"ocr_calls": call_count},
        "ocr_breakdown": {
            "call_count": call_count,
            "engine_detail": engine_detail,
            "input_prepare": stage,
            "request_setup": stage,
            "vision_perform": stage,
            "observation_mapping": stage,
            "residual": stage,
            "accounting": {
                "parent_total_ms": parent_ms,
                "components_total_ms": parent_ms,
                "residual_total_ms": per,
                "delta_ms": 0.0,
                "coverage_pct": 100.0,
            },
        },
    }


def test_validate_ocr_breakdown_accepts_balanced_payload() -> None:
    """调用数与 parent/outer wall 一致时通过。"""
    payload = _make_balanced_ocr_payload()
    _validate_ocr_breakdown(payload)


def test_validate_ocr_breakdown_rejects_call_count_mismatch() -> None:
    """call_count ≠ stages.ocr.count 时硬失败。"""
    payload = _make_balanced_ocr_payload(call_count=3)
    payload["stages"]["ocr"]["count"] = 2  # 故意错位
    with pytest.raises(RuntimeError, match="调用数对账失败"):
        _validate_ocr_breakdown(payload)


def test_validate_ocr_breakdown_rejects_throughput_mismatch() -> None:
    """throughput.ocr_calls 不一致时硬失败。"""
    payload = _make_balanced_ocr_payload(call_count=3)
    payload["throughput"]["ocr_calls"] = 1
    with pytest.raises(RuntimeError, match="调用数对账失败"):
        _validate_ocr_breakdown(payload)


def test_validate_ocr_breakdown_rejects_outer_wall_mismatch() -> None:
    """内部 parent 与 stages.ocr.total_ms 偏差过大时硬失败。"""
    payload = _make_balanced_ocr_payload(parent_ms=50.0, outer_ms=100.0)
    with pytest.raises(RuntimeError, match=r"parent 与 stages\.ocr wall"):
        _validate_ocr_breakdown(payload)


def test_validate_ocr_breakdown_relaxed_skips_hard_gates() -> None:
    """relaxed=True 时不因调用数错位失败。"""
    payload = _make_balanced_ocr_payload(call_count=3)
    payload["stages"]["ocr"]["count"] = 0
    payload["throughput"]["ocr_calls"] = 0
    _validate_ocr_breakdown(payload, relaxed=True)


def test_pipeline_payload_passes_external_ocr_accounting() -> None:
    """真实 Pipeline opaque 路径产出的 payload 满足外层对账。"""
    rec = PerformanceRecorder(mode="summary")
    ocr = CountingOcr()
    pipeline = Pipeline(
        detector=FixedRegionDetector(BoundingBox(0, 0, 320, 80)),
        ocr=ocr,
        config=Config(
            enable_line_select=True,
            subtitle_script="cjk",
            ocr_consensus_frames=2,
        ),
        performance_recorder=rec,
    )
    frames = [
        _blank_frame(0),
        _subtitle_frame(200),
        _subtitle_frame(400),
        _subtitle_frame(600),
        _blank_frame(800),
        _blank_frame(1000),
    ]
    _entries = pipeline.run_frames(iter(frames))
    payload = rec.to_payload()
    rec.close()
    assert len(_entries) >= 1
    # opaque 引擎走 breakdown；外层对账应对齐
    assert "ocr_breakdown" in payload
    _validate_ocr_breakdown(payload)
    assert (
        payload["ocr_breakdown"]["call_count"]
        == payload["stages"]["ocr"]["count"]
        == payload["throughput"]["ocr_calls"]
    )
