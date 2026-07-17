"""feat-037b/c: Pipeline/Extractor 埋点与 off/summary/trace 一致性。"""

from __future__ import annotations

import json
import subprocess
from pathlib import Path

import numpy as np
import pytest
from benchmark.report import write_reports
from benchmark.runner import RunConfig, run_benchmark
from PIL import Image

from sublift.config import Config
from sublift.detector import FixedRegionDetector
from sublift.diagnostics.performance import PerformanceMode, PerformanceRecorder
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
