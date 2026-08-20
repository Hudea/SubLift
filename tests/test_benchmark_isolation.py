"""Scoring/report imports must not load Oracle Pipeline, OCR, OpenCV or Pillow."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import pytest

from sublift.benchmark.config import RunConfig
from sublift.benchmark.runner import run_benchmark

_ISOLATION_SNIPPET = """
import sys
from sublift.benchmark import diagnostics, matrix_report, report, result, score, srt
from sublift.benchmark.score import align_existing_srt
from sublift.benchmark.report import write_reports

forbidden = (
    "cv2",
    "PIL",
    "PIL.Image",
    "sublift.pipeline",
    "sublift.pipeline.core",
    "sublift.ocr",
    "sublift.extractor",
    "sublift.detector",
    "sublift.benchmark.runner",
)
loaded = [name for name in forbidden if name in sys.modules]
assert not loaded, loaded
"""


def test_scoring_layers_do_not_import_oracle_runtime() -> None:
    completed = subprocess.run(
        [sys.executable, "-c", _ISOLATION_SNIPPET],
        cwd=Path(__file__).resolve().parents[1],
        capture_output=True,
        text=True,
        check=False,
    )
    assert completed.returncode == 0, completed.stdout + completed.stderr


def test_native_backend_requires_cli(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    video = tmp_path / "movie.mp4"
    video.write_bytes(b"fixture")
    gt = tmp_path / "gt.srt"
    gt.write_text("1\n00:00:00,000 --> 00:00:01,000\nhello\n\n", encoding="utf-8")
    monkeypatch.setattr("sublift.benchmark.runner.resolve_native_cli", lambda: None)
    config = RunConfig(
        video_path=video,
        ground_truth_path=gt,
        output_dir=tmp_path / "out",
        video_duration_seconds=1.0,
        isolate_processes=False,
    )
    with pytest.raises(RuntimeError, match="Native CLI"):
        run_benchmark(config)


def test_native_backend_rejects_region_box(tmp_path: Path) -> None:
    video = tmp_path / "movie.mp4"
    video.write_bytes(b"fixture")
    gt = tmp_path / "gt.srt"
    gt.write_text("1\n00:00:00,000 --> 00:00:01,000\nhello\n\n", encoding="utf-8")
    config = RunConfig(
        video_path=video,
        ground_truth_path=gt,
        output_dir=tmp_path / "out",
        video_duration_seconds=1.0,
        isolate_processes=False,
        region_box=(0, 10, 100, 20),
    )
    with pytest.raises(RuntimeError, match="region_box"):
        run_benchmark(config)


def test_native_backend_invokes_cli(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    video = tmp_path / "movie.mp4"
    video.write_bytes(b"fixture")
    gt = tmp_path / "gt.srt"
    gt.write_text("1\n00:00:00,000 --> 00:00:01,000\nhello\n\n", encoding="utf-8")
    cli = tmp_path / "sublift"
    cli.write_text("", encoding="utf-8")
    captured: list[list[str]] = []

    def fake_run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
        del kwargs
        captured.append(command)
        output = Path(command[command.index("-o") + 1])
        output.write_text(gt.read_text(encoding="utf-8"), encoding="utf-8")
        return subprocess.CompletedProcess(command, 0, stdout="", stderr="")

    monkeypatch.setattr("sublift.benchmark.runner.resolve_native_cli", lambda: cli)
    monkeypatch.setattr("sublift.benchmark.runner.resolve_worker_bin", lambda: None)
    monkeypatch.setattr("sublift.benchmark.runner.subprocess.run", fake_run)

    config = RunConfig(
        video_path=video,
        ground_truth_path=gt,
        engine="mock",
        output_dir=tmp_path / "out",
        video_duration_seconds=1.0,
        isolate_processes=False,
    )
    result = run_benchmark(config)
    assert captured
    assert captured[0][0] == str(cli)
    assert captured[0][1] == "extract"
    assert result.detected[0].text == "hello"
    assert result.exported_srt_path is not None
    assert result.exported_srt_path.exists()


def test_python_module_sublift_is_not_product_cli() -> None:
    completed = subprocess.run(
        [sys.executable, "-m", "sublift"],
        cwd=Path(__file__).resolve().parents[1],
        capture_output=True,
        text=True,
        check=False,
    )
    assert completed.returncode == 2
    assert "Native CLI" in completed.stderr
    assert "sublift-benchmark" in completed.stderr


def test_python_module_sublift_offline_is_benchmark_cli() -> None:
    completed = subprocess.run(
        [sys.executable, "-m", "sublift_offline", "--help"],
        cwd=Path(__file__).resolve().parents[1],
        capture_output=True,
        text=True,
        check=False,
    )
    assert completed.returncode == 0
    assert "sublift-benchmark" in completed.stdout
