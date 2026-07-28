"""Tests for benchmark git utilities and auto-increment label resolution."""

from __future__ import annotations

from pathlib import Path

from benchmark.git_utils import (
    clean_commit_message,
    get_latest_commit_message,
    resolve_auto_increment_label,
)


def test_clean_commit_message() -> None:
    assert clean_commit_message("") == "run"
    assert clean_commit_message(None) == "run"  # type: ignore[arg-type]
    assert clean_commit_message("feat(ocr): 字幕层筛选") == "字幕层筛选"
    assert clean_commit_message("fix(pipeline): 修复打轴问题") == "修复打轴问题"
    assert clean_commit_message("feat(ocr): 字幕层筛选，按 profile") == "字幕层筛选按"
    assert clean_commit_message("docs(phase3): feat-028 基线入库") == "feat02"
    assert clean_commit_message("feat: 123456789") == "123456"
    assert clean_commit_message("Refactor timeline logic") == "Refact"
    assert clean_commit_message("!!!") == "run"


def test_get_latest_commit_message_runs_successfully() -> None:
    # This should run in the git repository and return the last commit message or empty string
    msg = get_latest_commit_message()
    assert isinstance(msg, str)


def test_resolve_auto_increment_label_empty_dir(tmp_path: Path) -> None:
    final_label, final_output_dir = resolve_auto_increment_label(tmp_path, "字幕层筛选")
    assert final_label == "字幕层筛选_1"
    assert final_output_dir == tmp_path / "字幕层筛选_1"


def test_resolve_auto_increment_label_incrementing(tmp_path: Path) -> None:
    # Create existing directories
    (tmp_path / "字幕层筛选_1").mkdir()
    (tmp_path / "字幕层筛选_2").mkdir()
    (tmp_path / "other_label_1").mkdir()

    final_label, final_output_dir = resolve_auto_increment_label(tmp_path, "字幕层筛选")
    assert final_label == "字幕层筛选_3"
    assert final_output_dir == tmp_path / "字幕层筛选_3"


def test_resolve_auto_increment_label_with_files(tmp_path: Path) -> None:
    # Create a directory and a file
    (tmp_path / "字幕层筛选_1").mkdir()
    (tmp_path / "字幕层筛选_2.txt").write_text("dummy", encoding="utf-8")

    final_label, final_output_dir = resolve_auto_increment_label(tmp_path, "字幕层筛选")
    assert final_label == "字幕层筛选_3"
    assert final_output_dir == tmp_path / "字幕层筛选_3"


def test_resolve_auto_increment_label_non_consecutive(tmp_path: Path) -> None:
    (tmp_path / "字幕层筛选_1").mkdir()
    (tmp_path / "字幕层筛选_5").mkdir()

    final_label, final_output_dir = resolve_auto_increment_label(tmp_path, "字幕层筛选")
    assert final_label == "字幕层筛选_6"
    assert final_output_dir == tmp_path / "字幕层筛选_6"
