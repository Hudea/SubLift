"""Benchmark manifest loader tests."""

from __future__ import annotations

import json
from pathlib import Path

import pytest
from benchmark.manifest import ManifestError, load_run_config


def test_load_run_config_resolves_paths_from_repo_root(tmp_path: Path) -> None:
    repo = tmp_path / "repo"
    manifest_dir = repo / "benchmark" / "manifests"
    manifest_dir.mkdir(parents=True)
    (repo / "pyproject.toml").write_text("", encoding="utf-8")
    (repo / "feature-list.json").write_text("{}", encoding="utf-8")

    manifest = manifest_dir / "run.json"
    manifest.write_text(
        json.dumps(
            {
                "video": "debug/movie.mp4",
                "ground_truth": "benchmark/fixtures/movie.srt",
                "fps": 8,
                "engine": "vision",
                "confidence": 0.7,
                "subtitle_script": "cjk",
                "match_threshold": 0.6,
                "region_box": [0, 842, 1920, 126],
                "label": "gui_region_8fps",
                "video_duration_seconds": 254.272,
                "output_dir": "debug/benchmark-reports",
            }
        ),
        encoding="utf-8",
    )

    config = load_run_config(manifest)

    assert config.video_path == repo / "debug/movie.mp4"
    assert config.ground_truth_path == repo / "benchmark/fixtures/movie.srt"
    assert config.fps == 8.0
    assert config.confidence == 0.7
    assert config.subtitle_script == "cjk"
    assert config.match_threshold == 0.6
    assert config.region_box == (0, 842, 1920, 126)
    assert config.label == "gui_region_8fps"
    assert config.video_duration_seconds == 254.272
    assert config.output_dir == repo / "debug/benchmark-reports"


def test_load_run_config_rejects_invalid_region_box(tmp_path: Path) -> None:
    manifest = tmp_path / "run.json"
    manifest.write_text(
        json.dumps(
            {
                "video": "debug/movie.mp4",
                "ground_truth": "benchmark/fixtures/movie.srt",
                "region_box": [0, 10, 1920, 0],
            }
        ),
        encoding="utf-8",
    )

    with pytest.raises(ManifestError, match="width/height"):
        load_run_config(manifest)


def test_load_run_config_rejects_invalid_script(tmp_path: Path) -> None:
    manifest = tmp_path / "run.json"
    manifest.write_text(
        json.dumps(
            {
                "video": "debug/movie.mp4",
                "ground_truth": "benchmark/fixtures/movie.srt",
                "subtitle_script": "emoji",
            }
        ),
        encoding="utf-8",
    )

    with pytest.raises(ManifestError, match="subtitle_script"):
        load_run_config(manifest)


def test_load_run_config_default_performance_off(tmp_path: Path) -> None:
    """旧 manifest 无 performance 块时默认为 off，不改变行为。"""
    repo = tmp_path / "repo"
    manifest_dir = repo / "benchmark" / "manifests"
    manifest_dir.mkdir(parents=True)
    (repo / "pyproject.toml").write_text("", encoding="utf-8")
    (repo / "feature-list.json").write_text("{}", encoding="utf-8")
    manifest = manifest_dir / "run.json"
    manifest.write_text(
        json.dumps(
            {
                "video": "debug/movie.mp4",
                "ground_truth": "benchmark/fixtures/movie.srt",
            }
        ),
        encoding="utf-8",
    )
    config = load_run_config(manifest)
    assert config.performance_mode == "off"
    assert config.warmup_runs == 0
    assert config.measured_runs == 1


def test_load_run_config_performance_block(tmp_path: Path) -> None:
    repo = tmp_path / "repo"
    manifest_dir = repo / "benchmark" / "manifests"
    manifest_dir.mkdir(parents=True)
    (repo / "pyproject.toml").write_text("", encoding="utf-8")
    (repo / "feature-list.json").write_text("{}", encoding="utf-8")
    manifest = manifest_dir / "run.json"
    manifest.write_text(
        json.dumps(
            {
                "video": "debug/movie.mp4",
                "ground_truth": "benchmark/fixtures/movie.srt",
                "performance": {
                    "mode": "summary",
                    "warmup_runs": 1,
                    "measured_runs": 3,
                },
            }
        ),
        encoding="utf-8",
    )
    config = load_run_config(manifest)
    assert config.performance_mode == "summary"
    assert config.warmup_runs == 1
    assert config.measured_runs == 3


def test_load_run_config_rejects_invalid_performance_mode(tmp_path: Path) -> None:
    manifest = tmp_path / "run.json"
    manifest.write_text(
        json.dumps(
            {
                "video": "debug/movie.mp4",
                "ground_truth": "benchmark/fixtures/movie.srt",
                "performance": {"mode": "debug"},
            }
        ),
        encoding="utf-8",
    )
    with pytest.raises(ManifestError, match=r"performance\.mode"):
        load_run_config(manifest)


def test_load_run_config_rejects_invalid_measured_runs(tmp_path: Path) -> None:
    manifest = tmp_path / "run.json"
    manifest.write_text(
        json.dumps(
            {
                "video": "debug/movie.mp4",
                "ground_truth": "benchmark/fixtures/movie.srt",
                "performance": {"mode": "summary", "measured_runs": 0},
            }
        ),
        encoding="utf-8",
    )
    with pytest.raises(ManifestError, match="measured_runs"):
        load_run_config(manifest)
