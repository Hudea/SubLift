"""Unified benchmark matrix and CLI tests."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from sublift.benchmark.cli import main
from sublift.benchmark.config import ManifestError, RunConfig, load_manifest
from sublift.benchmark.matrix import expand_matrix, parse_set_args, parse_vary_args
from sublift.benchmark.matrix_report import completed_record, write_matrix_reports
from sublift.benchmark.result import RunResult
from sublift.benchmark.srt import SrtEntry


def _write_config(tmp_path: Path, *, output_dir: Path | None = None) -> Path:
    repo = tmp_path / "repo"
    config_dir = repo / "benchmark" / "configs"
    config_dir.mkdir(parents=True)
    (repo / "pyproject.toml").write_text("", encoding="utf-8")
    (repo / "phases.json").write_text("{}", encoding="utf-8")
    payload = {
        "schema_version": 2,
        "run": {
            "video": "debug/movie.mp4",
            "ground_truth": "benchmark/datasets/movie.srt",
            "label": "test_matrix",
            "output_dir": str(output_dir) if output_dir else "debug/benchmark/runs",
        },
        "matrix": {"fps": [5, 8]},
    }
    path = config_dir / "matrix.json"
    path.write_text(json.dumps(payload), encoding="utf-8")
    return path


def test_parse_generic_set_and_vary_values() -> None:
    assert parse_set_args(
        [
            "fps=8",
            "engine=paddle",
            "performance.mode=summary",
            "isolate_processes=false",
        ]
    ) == {
        "fps": 8,
        "engine": "paddle",
        "performance.mode": "summary",
        "isolate_processes": False,
    }
    assert parse_vary_args(["fps=[5,8,12]", "engine=vision,paddle"]) == {
        "fps": [5, 8, 12],
        "engine": ["vision", "paddle"],
    }


def test_expand_matrix_is_deterministic_and_bounded(tmp_path: Path) -> None:
    document = load_manifest(_write_config(tmp_path))

    cells = expand_matrix(
        document,
        cli_axes={"engine": ["vision", "paddle"]},
        base_label="matrix",
        max_cases=4,
    )

    assert [cell.label for cell in cells] == [
        "matrix__fps-5__engine-vision",
        "matrix__fps-5__engine-paddle",
        "matrix__fps-8__engine-vision",
        "matrix__fps-8__engine-paddle",
    ]
    assert [cell.config.engine for cell in cells] == [
        "vision",
        "paddle",
        "vision",
        "paddle",
    ]

    with pytest.raises(ManifestError, match="超过"):
        expand_matrix(
            document,
            cli_axes={"engine": ["vision", "paddle"]},
            base_label="matrix",
            max_cases=3,
        )


def test_expand_matrix_supports_nested_pipeline_axis(tmp_path: Path) -> None:
    document = load_manifest(_write_config(tmp_path))

    cells = expand_matrix(
        document,
        cli_axes={"pipeline.change_point.hysteresis_frames": [1, 2]},
        base_label="pipeline",
    )

    assert [cell.config.pipeline_overrides for cell in cells] == [
        {"change_point": {"hysteresis_frames": 1}},
        {"change_point": {"hysteresis_frames": 2}},
        {"change_point": {"hysteresis_frames": 1}},
        {"change_point": {"hysteresis_frames": 2}},
    ]


def test_cli_matrix_dry_run_does_not_write(
    tmp_path: Path,
    capsys: pytest.CaptureFixture[str],
) -> None:
    output_dir = tmp_path / "output"
    config = _write_config(tmp_path, output_dir=output_dir)

    exit_code = main(
        [
            "matrix",
            str(config),
            "--vary",
            'engine=["vision","paddle"]',
            "--dry-run",
        ]
    )

    assert exit_code == 0
    assert not output_dir.exists()
    payload = json.loads(capsys.readouterr().out)
    assert payload["case_count"] == 4
    assert payload["cells"][0]["config"]["pipeline"] == {}
    assert "pipeline_overrides" not in payload["cells"][0]["config"]


def test_cli_score_existing_srt(tmp_path: Path) -> None:
    output_dir = tmp_path / "output"
    config = _write_config(tmp_path, output_dir=output_dir)
    repo = config.parents[2]
    gt = repo / "benchmark/datasets/movie.srt"
    gt.parent.mkdir(parents=True)
    gt.write_text(
        "1\n00:00:01,000 --> 00:00:02,000\nhello\n",
        encoding="utf-8",
    )
    detected = tmp_path / "detected.srt"
    detected.write_text(gt.read_text(encoding="utf-8"), encoding="utf-8")

    exit_code = main(
        [
            "score",
            str(config),
            str(detected),
            "--label",
            "scored",
            "--duration",
            "10",
            "--elapsed",
            "1",
        ]
    )

    assert exit_code == 0
    report_dir = output_dir / "scored"
    assert len(list(report_dir.glob("*.agent.json"))) == 1
    assert len(list(report_dir.glob("*.summary.md"))) == 1


def test_matrix_aggregate_reports(tmp_path: Path) -> None:
    entry = SrtEntry(index=1, start_ms=1000, end_ms=2000, text="hello")
    result = RunResult(
        config=RunConfig(
            video_path=tmp_path / "movie.mp4",
            ground_truth_path=tmp_path / "gt.srt",
            label="cell",
        ),
        detected=[entry],
        ground_truth=[entry],
        elapsed_seconds=1.0,
        video_duration_seconds=10.0,
    )
    record = completed_record(
        label="cell",
        overrides={"fps": 8},
        result=result,
        artifacts={"agent_json": tmp_path / "cell.agent.json"},
    )

    paths = write_matrix_reports([record], tmp_path / "matrix")

    assert record.status == "pass"
    assert record.metrics["timing_f1"] == 1.0
    assert all(path.exists() for path in paths.values())
