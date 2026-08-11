"""Benchmark manifest loader tests."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from sublift.benchmark.config import (
    ManifestError,
    discover_repo_root,
    load_manifest,
    load_run_config,
    resolve_run_config,
)
from sublift.benchmark.pipeline_config import apply_pipeline_overrides
from sublift.config import Config


def test_load_run_config_resolves_paths_from_repo_root(tmp_path: Path) -> None:
    repo = tmp_path / "repo"
    manifest_dir = repo / "benchmark" / "manifests"
    manifest_dir.mkdir(parents=True)
    (repo / "pyproject.toml").write_text("", encoding="utf-8")
    (repo / "phases.json").write_text("{}", encoding="utf-8")

    manifest = manifest_dir / "run.json"
    manifest.write_text(
        json.dumps(
            {
                "video": "debug/movie.mp4",
                "ground_truth": "benchmark/datasets/movie.srt",
                "fps": 8,
                "engine": "vision",
                "confidence": 0.7,
                "subtitle_script": "cjk",
                "match_threshold": 0.6,
                "region_box": [0, 842, 1920, 126],
                "label": "gui_region_8fps",
                "video_duration_seconds": 254.272,
                "output_dir": "debug/benchmark/archive/legacy-runs",
            }
        ),
        encoding="utf-8",
    )

    config = load_run_config(manifest)

    assert config.video_path == repo / "debug/movie.mp4"
    assert config.ground_truth_path == repo / "benchmark/datasets/movie.srt"
    assert config.fps == 8.0
    assert config.confidence == 0.7
    assert config.subtitle_script == "cjk"
    assert config.match_threshold == 0.6
    assert config.region_box == (0, 842, 1920, 126)
    assert config.label == "gui_region_8fps"
    assert config.video_duration_seconds == 254.272
    assert config.output_dir == repo / "debug/benchmark/archive/legacy-runs"


def test_load_run_config_keeps_feature_list_marker_as_legacy_fallback(tmp_path: Path) -> None:
    repo = tmp_path / "legacy-repo"
    manifest_dir = repo / "benchmark" / "manifests"
    manifest_dir.mkdir(parents=True)
    (repo / "pyproject.toml").write_text("", encoding="utf-8")
    (repo / "feature-list.json").write_text("{}", encoding="utf-8")
    manifest = manifest_dir / "run.json"
    manifest.write_text(
        json.dumps(
            {
                "video": "debug/movie.mp4",
                "ground_truth": "benchmark/datasets/movie.srt",
            }
        ),
        encoding="utf-8",
    )

    config = load_run_config(manifest)

    assert config.video_path == repo / "debug/movie.mp4"
    assert config.ground_truth_path == repo / "benchmark/datasets/movie.srt"


def test_discover_repo_root_falls_back_to_cwd_without_markers(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    manifest = tmp_path / "orphan" / "run.json"
    manifest.parent.mkdir(parents=True)
    manifest.write_text("{}", encoding="utf-8")
    fallback = tmp_path / "fallback"
    fallback.mkdir()
    monkeypatch.chdir(fallback)

    assert discover_repo_root(manifest) == fallback


def test_load_run_config_rejects_invalid_region_box(tmp_path: Path) -> None:
    manifest = tmp_path / "run.json"
    manifest.write_text(
        json.dumps(
            {
                "video": "debug/movie.mp4",
                "ground_truth": "benchmark/datasets/movie.srt",
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
                "ground_truth": "benchmark/datasets/movie.srt",
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
    (repo / "phases.json").write_text("{}", encoding="utf-8")
    manifest = manifest_dir / "run.json"
    manifest.write_text(
        json.dumps(
            {
                "video": "debug/movie.mp4",
                "ground_truth": "benchmark/datasets/movie.srt",
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
    (repo / "phases.json").write_text("{}", encoding="utf-8")
    manifest = manifest_dir / "run.json"
    manifest.write_text(
        json.dumps(
            {
                "video": "debug/movie.mp4",
                "ground_truth": "benchmark/datasets/movie.srt",
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
                "ground_truth": "benchmark/datasets/movie.srt",
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
                "ground_truth": "benchmark/datasets/movie.srt",
                "performance": {"mode": "summary", "measured_runs": 0},
            }
        ),
        encoding="utf-8",
    )
    with pytest.raises(ManifestError, match="measured_runs"):
        load_run_config(manifest)


def test_v2_document_supports_matrix_and_dotted_overrides(tmp_path: Path) -> None:
    repo = tmp_path / "repo"
    config_dir = repo / "benchmark" / "configs"
    config_dir.mkdir(parents=True)
    (repo / "pyproject.toml").write_text("", encoding="utf-8")
    (repo / "phases.json").write_text("{}", encoding="utf-8")
    manifest = config_dir / "matrix.json"
    manifest.write_text(
        json.dumps(
            {
                "schema_version": 2,
                "run": {
                    "video": "debug/movie.mp4",
                    "ground_truth": "benchmark/datasets/movie.srt",
                    "engine": "vision",
                },
                "matrix": {
                    "fps": [5, 8],
                    "engine": ["vision", "paddle"],
                },
            }
        ),
        encoding="utf-8",
    )

    document = load_manifest(manifest)
    config = resolve_run_config(
        document,
        overrides={
            "fps": 12,
            "engine": "paddle",
            "performance.mode": "summary",
            "performance.measured_runs": 3,
        },
    )

    assert document.matrix == {
        "fps": [5, 8],
        "engine": ["vision", "paddle"],
    }
    assert config.fps == 12.0
    assert config.engine == "paddle"
    assert config.performance_mode == "summary"
    assert config.measured_runs == 3


def test_pipeline_overrides_are_validated_and_applied(tmp_path: Path) -> None:
    repo = tmp_path / "repo"
    config_dir = repo / "benchmark" / "configs"
    config_dir.mkdir(parents=True)
    (repo / "pyproject.toml").write_text("", encoding="utf-8")
    (repo / "phases.json").write_text("{}", encoding="utf-8")
    manifest = config_dir / "pipeline.json"
    manifest.write_text(
        json.dumps(
            {
                "schema_version": 2,
                "run": {
                    "video": "debug/movie.mp4",
                    "ground_truth": "benchmark/datasets/movie.srt",
                    "pipeline": {
                        "min_duration_ms": 300,
                        "change_point": {"presence_threshold": 0.02},
                    },
                },
            }
        ),
        encoding="utf-8",
    )

    config = load_run_config(manifest)
    resolved = apply_pipeline_overrides(Config(), config.pipeline_overrides)

    assert config.pipeline_overrides == {
        "min_duration_ms": 300,
        "change_point": {"presence_threshold": 0.02},
    }
    assert resolved.min_duration_ms == 300
    assert resolved.change_point.presence_threshold == 0.02


def test_pipeline_overrides_reject_unknown_and_reserved_fields(tmp_path: Path) -> None:
    manifest = tmp_path / "run.json"
    manifest.write_text(
        json.dumps(
            {
                "video": "debug/movie.mp4",
                "ground_truth": "benchmark/datasets/movie.srt",
                "pipeline": {"sample_fps": 12, "typo": 1},
            }
        ),
        encoding="utf-8",
    )

    with pytest.raises(ManifestError, match=r"未知或保留字段.*sample_fps.*typo"):
        load_run_config(manifest)


@pytest.mark.parametrize(
    ("pipeline", "message"),
    [
        ({"change_point": {"hysteresis_frames": 0}}, "必须大于 0"),
        ({"change_point": {"presence_threshold": 1.1}}, "0 到 1"),
        ({"change_point": {"ssim_window_size": 6}}, "必须是奇数"),
        ({"merge_gap_ms": -1}, "不能为负"),
    ],
)
def test_pipeline_overrides_reject_invalid_ranges(
    tmp_path: Path,
    pipeline: dict[str, object],
    message: str,
) -> None:
    manifest = tmp_path / "run.json"
    manifest.write_text(
        json.dumps(
            {
                "video": "debug/movie.mp4",
                "ground_truth": "benchmark/datasets/movie.srt",
                "pipeline": pipeline,
            }
        ),
        encoding="utf-8",
    )

    with pytest.raises(ManifestError, match=message):
        load_run_config(manifest)


def test_load_run_config_rejects_unknown_fields(tmp_path: Path) -> None:
    manifest = tmp_path / "run.json"
    manifest.write_text(
        json.dumps(
            {
                "video": "debug/movie.mp4",
                "ground_truth": "benchmark/datasets/movie.srt",
                "fpps": 8,
            }
        ),
        encoding="utf-8",
    )

    with pytest.raises(ManifestError, match=r"未知字段.*fpps"):
        load_run_config(manifest)


def test_default_output_uses_unified_debug_tree(tmp_path: Path) -> None:
    repo = tmp_path / "repo"
    config_dir = repo / "benchmark" / "configs"
    config_dir.mkdir(parents=True)
    (repo / "pyproject.toml").write_text("", encoding="utf-8")
    (repo / "phases.json").write_text("{}", encoding="utf-8")
    manifest = config_dir / "run.json"
    manifest.write_text(
        json.dumps(
            {
                "video": "debug/movie.mp4",
                "ground_truth": "benchmark/datasets/movie.srt",
            }
        ),
        encoding="utf-8",
    )

    config = load_run_config(manifest)

    assert config.output_dir == repo / "debug/benchmark/runs"
