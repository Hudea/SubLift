"""Safety tests for the fixed-path local artifact cleanup entrypoint."""

from __future__ import annotations

from pathlib import Path

import pytest
from scripts.cleanup_local_artifacts import (
    ArtifactClass,
    CleanupCandidate,
    CleanupTarget,
    apply_candidates,
    build_parser,
    inspect_targets,
    render_plan,
    selected_classes,
)


def _target(artifact_class: ArtifactClass, relative_path: str) -> CleanupTarget:
    return CleanupTarget(artifact_class, Path(relative_path))


def _plan(
    repo_root: Path, artifact_class: ArtifactClass, relative_path: str
) -> list[CleanupCandidate]:
    return inspect_targets(
        repo_root,
        [artifact_class],
        targets=[_target(artifact_class, relative_path)],
    )


def test_default_generated_dry_run_preserves_files_and_reports_absolute_path(
    tmp_path: Path,
) -> None:
    frame = tmp_path / "debug" / "frames" / "frame.png"
    frame.parent.mkdir(parents=True)
    frame.write_bytes(b"frame")

    candidates = _plan(tmp_path, ArtifactClass.GENERATED, "debug/frames")
    report = render_plan(tmp_path, candidates, apply=False)

    assert frame.read_bytes() == b"frame"
    assert candidates[0].state == "ready"
    assert candidates[0].reclaimable_bytes == 5
    assert str((tmp_path / "debug/frames").resolve()) in report
    assert "DRY-RUN（不写盘）" in report


def test_default_selection_only_targets_recommended_generated_artifacts() -> None:
    args = build_parser().parse_args([])

    assert selected_classes(args) == frozenset({ArtifactClass.GENERATED})


def test_explicit_category_flags_do_not_cross_select() -> None:
    for flag, expected in (
        ("--generated", ArtifactClass.GENERATED),
        ("--swift-build", ArtifactClass.SWIFT_BUILD),
        ("--cpp-build", ArtifactClass.CPP_BUILD),
        ("--python-env", ArtifactClass.PYTHON_ENV),
    ):
        args = build_parser().parse_args([flag])
        assert selected_classes(args) == frozenset({expected})


def test_rejects_path_escape_repo_root_and_git_metadata(tmp_path: Path) -> None:
    outside = tmp_path.parent / "outside"
    outside.mkdir(exist_ok=True)
    (tmp_path / ".git").mkdir()
    targets = [
        _target(ArtifactClass.GENERATED, "../outside"),
        _target(ArtifactClass.GENERATED, "."),
        _target(ArtifactClass.GENERATED, ".git"),
    ]

    candidates = inspect_targets(tmp_path, [ArtifactClass.GENERATED], targets=targets)

    assert [candidate.state for candidate in candidates] == ["protected", "protected", "protected"]
    assert "路径逃逸" in (candidates[0].reason or "")
    assert "仓库根" in (candidates[1].reason or "")
    assert "Git 元数据" in (candidates[2].reason or "")


def test_rejects_symbolic_link_target(tmp_path: Path) -> None:
    outside = tmp_path.parent / "cleanup-symlink-outside"
    outside.mkdir(exist_ok=True)
    debug = tmp_path / "debug"
    debug.mkdir()
    (debug / "frames").symlink_to(outside, target_is_directory=True)

    candidate = _plan(tmp_path, ArtifactClass.GENERATED, "debug/frames")[0]

    assert candidate.state == "protected"
    assert "符号链接" in (candidate.reason or "")


def test_protects_media_srt_and_gt_files(tmp_path: Path) -> None:
    for filename in ("input.mp4", "subtitle.srt", "reference.gt"):
        protected = tmp_path / "debug" / "frames" / filename
        protected.parent.mkdir(parents=True, exist_ok=True)
        protected.write_text("protected", encoding="utf-8")

        candidate = _plan(tmp_path, ArtifactClass.GENERATED, "debug/frames")[0]

        assert candidate.state == "protected"
        assert protected.exists()
        protected.unlink()


def test_protects_unknown_generated_files(tmp_path: Path) -> None:
    unknown = tmp_path / "debug" / "frames" / "unclassified.bin"
    unknown.parent.mkdir(parents=True)
    unknown.write_bytes(b"unknown")

    candidate = _plan(tmp_path, ArtifactClass.GENERATED, "debug/frames")[0]

    assert candidate.state == "protected"
    assert "未分类文件" in (candidate.reason or "")
    assert apply_candidates(tmp_path, [candidate]) == []
    assert unknown.exists()


def test_empty_candidate_and_repeated_apply_are_safe_in_temporary_root(tmp_path: Path) -> None:
    empty = tmp_path / "debug" / "frames"
    empty.mkdir(parents=True)
    empty_candidate = _plan(tmp_path, ArtifactClass.GENERATED, "debug/frames")[0]
    assert empty_candidate.state == "ready"
    assert empty_candidate.reclaimable_bytes == 0

    assert apply_candidates(tmp_path, [empty_candidate]) == [empty]
    assert not empty.exists()
    missing_candidate = _plan(tmp_path, ArtifactClass.GENERATED, "debug/frames")[0]
    assert missing_candidate.state == "missing"
    assert apply_candidates(tmp_path, [missing_candidate]) == []


def test_unreadable_directory_is_protected(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    frames = tmp_path / "debug" / "frames"
    frames.mkdir(parents=True)
    (frames / "frame.png").write_bytes(b"frame")

    def failing_rglob(self: Path, pattern: str) -> None:
        del self, pattern
        raise OSError("permission denied")

    monkeypatch.setattr(Path, "rglob", failing_rglob)
    candidate = _plan(tmp_path, ArtifactClass.GENERATED, "debug/frames")[0]

    assert candidate.state == "protected"
    assert "无法检查目录" in (candidate.reason or "")
    assert apply_candidates(tmp_path, [candidate]) == []
    assert (frames / "frame.png").exists()


def test_category_flags_do_not_cross_select_targets(tmp_path: Path) -> None:
    targets = [
        _target(ArtifactClass.GENERATED, "debug/frames"),
        _target(ArtifactClass.SWIFT_BUILD, "apps/macos/.build"),
        _target(ArtifactClass.CPP_BUILD, "build"),
        _target(ArtifactClass.PYTHON_ENV, ".venv"),
    ]
    for target in targets:
        directory = tmp_path / target.relative_path
        directory.mkdir(parents=True)
        (directory / "artifact.bin").write_bytes(b"x")

    for artifact_class, expected in (
        (ArtifactClass.GENERATED, "debug/frames"),
        (ArtifactClass.SWIFT_BUILD, "apps/macos/.build"),
        (ArtifactClass.CPP_BUILD, "build"),
        (ArtifactClass.PYTHON_ENV, ".venv"),
    ):
        candidates = inspect_targets(tmp_path, [artifact_class], targets=targets)
        assert [candidate.target.relative_path.as_posix() for candidate in candidates] == [expected]
