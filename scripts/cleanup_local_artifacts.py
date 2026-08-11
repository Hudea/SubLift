#!/usr/bin/env python3
"""Preview or explicitly remove only classified local SubLift artifacts.

The default is a read-only dry run over the recommended L1 generated-artifact
targets. ``--apply`` is intentionally explicit; running it against a real
checkout still requires the operator's separate approval.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from collections.abc import Iterable, Sequence
from dataclasses import dataclass
from enum import StrEnum
from pathlib import Path
from typing import Literal


class ArtifactClass(StrEnum):
    GENERATED = "generated"
    SWIFT_BUILD = "swift-build"
    CPP_BUILD = "cpp-build"
    PYTHON_ENV = "python-env"


CandidateState = Literal["ready", "missing", "protected"]


@dataclass(frozen=True)
class CleanupTarget:
    artifact_class: ArtifactClass
    relative_path: Path


@dataclass(frozen=True)
class CleanupCandidate:
    target: CleanupTarget
    path: Path
    state: CandidateState
    reclaimable_bytes: int
    reason: str | None = None


_TARGETS: tuple[CleanupTarget, ...] = (
    CleanupTarget(ArtifactClass.GENERATED, Path("debug/frames")),
    CleanupTarget(ArtifactClass.GENERATED, Path("debug/failure_frames_feat034_p1_fix2")),
    CleanupTarget(ArtifactClass.GENERATED, Path("debug/benchmark/runs")),
    CleanupTarget(ArtifactClass.GENERATED, Path("debug/benchmark/perf")),
    CleanupTarget(ArtifactClass.GENERATED, Path("debug/diagnostics")),
    CleanupTarget(ArtifactClass.SWIFT_BUILD, Path("apps/macos/.build")),
    CleanupTarget(ArtifactClass.CPP_BUILD, Path("build")),
    CleanupTarget(ArtifactClass.PYTHON_ENV, Path(".venv")),
)

_PROTECTED_SUFFIXES = frozenset({".mp4", ".mkv", ".mov", ".srt", ".vtt", ".ass", ".gt"})
_GENERATED_SUFFIXES = frozenset(
    {".csv", ".jpeg", ".jpg", ".json", ".jsonl", ".log", ".md", ".png", ".txt", ".webp"}
)


class CleanupSafetyError(RuntimeError):
    """Raised when a cleanup target no longer meets the fixed-path contract."""


def resolve_current_repo_root(cwd: Path | None = None) -> Path:
    """Resolve the current checkout with Git instead of relying on a hard-coded path."""
    result = subprocess.run(
        ["git", "rev-parse", "--show-toplevel"],
        check=False,
        cwd=cwd,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        detail = result.stderr.strip() or "当前目录不在 Git checkout 中"
        raise CleanupSafetyError(detail)
    repo_root = Path(result.stdout.strip()).resolve()
    if not (repo_root / ".git").exists():
        raise CleanupSafetyError(f"Git 根目录不可信: {repo_root}")
    return repo_root


def selected_classes(args: argparse.Namespace) -> frozenset[ArtifactClass]:
    classes = {
        artifact_class
        for artifact_class, enabled in (
            (ArtifactClass.GENERATED, args.generated),
            (ArtifactClass.SWIFT_BUILD, args.swift_build),
            (ArtifactClass.CPP_BUILD, args.cpp_build),
            (ArtifactClass.PYTHON_ENV, args.python_env),
        )
        if enabled
    }
    return frozenset(classes or {ArtifactClass.GENERATED})


def inspect_targets(
    repo_root: Path,
    classes: Iterable[ArtifactClass],
    *,
    targets: Sequence[CleanupTarget] = _TARGETS,
) -> list[CleanupCandidate]:
    """Return an absolute, read-only cleanup plan for fixed classified targets."""
    resolved_root = repo_root.resolve()
    selected = frozenset(classes)
    return [
        inspect_target(resolved_root, target)
        for target in targets
        if target.artifact_class in selected
    ]


def inspect_target(repo_root: Path, target: CleanupTarget) -> CleanupCandidate:
    raw_path = repo_root / target.relative_path
    absolute_path = raw_path.resolve(strict=False)
    rejection = _target_rejection(repo_root, raw_path, absolute_path)
    if rejection is not None:
        return CleanupCandidate(target, absolute_path, "protected", 0, rejection)
    if not absolute_path.exists():
        return CleanupCandidate(target, absolute_path, "missing", 0)
    if not absolute_path.is_dir():
        return CleanupCandidate(target, absolute_path, "protected", 0, "目标不是可分类目录")

    try:
        protected_reason = _contents_rejection(absolute_path, target.artifact_class)
        if protected_reason is not None:
            return CleanupCandidate(target, absolute_path, "protected", 0, protected_reason)
        reclaimable = _directory_size(absolute_path)
    except OSError as exc:
        return CleanupCandidate(
            target,
            absolute_path,
            "protected",
            0,
            f"无法检查目录: {exc}",
        )
    return CleanupCandidate(
        target,
        absolute_path,
        "ready",
        reclaimable,
    )


def apply_candidates(repo_root: Path, candidates: Iterable[CleanupCandidate]) -> list[Path]:
    """Delete only a previously planned target after revalidating every boundary."""
    removed: list[Path] = []
    resolved_root = repo_root.resolve()
    for candidate in candidates:
        if candidate.state != "ready":
            continue
        refreshed = inspect_target(resolved_root, candidate.target)
        if refreshed.state != "ready" or refreshed.path != candidate.path:
            raise CleanupSafetyError(
                f"目标在执行前不再安全: {candidate.path} ({refreshed.reason or refreshed.state})"
            )
        shutil.rmtree(refreshed.path)
        removed.append(refreshed.path)
    return removed


def render_plan(repo_root: Path, candidates: Sequence[CleanupCandidate], *, apply: bool) -> str:
    mode = "APPLY（将删除）" if apply else "DRY-RUN（不写盘）"
    lines = [f"模式: {mode}", f"仓库根: {repo_root.resolve()}"]
    total = 0
    for candidate in candidates:
        if candidate.state == "ready":
            total += candidate.reclaimable_bytes
            detail = f"预计释放 {_format_size(candidate.reclaimable_bytes)}"
        elif candidate.state == "missing":
            detail = "不存在"
        else:
            detail = f"保护: {candidate.reason}"
        lines.append(
            f"- [{candidate.target.artifact_class.value}/{candidate.state}] "
            f"{candidate.path} — {detail}"
        )
    lines.append(f"合计预计释放: {_format_size(total)}")
    return "\n".join(lines)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="安全预览或清理固定的仓库内本地产物")
    parser.add_argument(
        "--generated",
        action="store_true",
        help="选择 L1 生成的抽帧、失败帧与当前 benchmark 报告（默认选择）",
    )
    parser.add_argument("--swift-build", action="store_true", help="额外选择 apps/macos/.build")
    parser.add_argument("--cpp-build", action="store_true", help="额外选择 build/")
    parser.add_argument(
        "--python-env",
        action="store_true",
        help="额外选择 .venv（不在推荐默认范围）",
    )
    parser.add_argument(
        "--apply",
        action="store_true",
        help="执行已显示的固定目标删除；真实 checkout 仍须先获得用户明确授权",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        repo_root = resolve_current_repo_root()
        candidates = inspect_targets(repo_root, selected_classes(args))
        print(render_plan(repo_root, candidates, apply=args.apply))
        if not args.apply:
            return 0
        removed = apply_candidates(repo_root, candidates)
    except CleanupSafetyError as exc:
        print(f"拒绝清理: {exc}", file=sys.stderr)
        return 2

    print(f"已删除 {len(removed)} 个已分类目录。")
    return 0


def _target_rejection(repo_root: Path, raw_path: Path, absolute_path: Path) -> str | None:
    if raw_path.is_symlink():
        return "拒绝符号链接目标"
    if absolute_path == repo_root:
        return "拒绝仓库根目录"
    try:
        relative = absolute_path.relative_to(repo_root)
    except ValueError:
        return "拒绝工作区外路径或路径逃逸"
    if not relative.parts or relative.parts[0] == ".git":
        return "拒绝 Git 元数据"
    return None


def _contents_rejection(path: Path, artifact_class: ArtifactClass) -> str | None:
    for child in path.rglob("*"):
        try:
            is_symlink = child.is_symlink()
            is_file = child.is_file()
        except OSError as exc:
            return f"无法检查路径: {child} ({exc})"
        if is_symlink:
            return f"拒绝包含符号链接: {child}"
        if not is_file:
            continue
        suffix = child.suffix.lower()
        if suffix in _PROTECTED_SUFFIXES:
            return f"保护媒体、SRT 或 GT: {child}"
        if artifact_class is ArtifactClass.GENERATED and suffix not in _GENERATED_SUFFIXES:
            return f"保护未分类文件: {child}"
    return None


def _directory_size(path: Path) -> int:
    total = 0
    for child in path.rglob("*"):
        try:
            if child.is_file() and not child.is_symlink():
                total += child.stat().st_size
        except OSError:
            # Unreadable children are handled as protected by inspect_target when
            # the walk itself fails; skip individual stat failures when sizing.
            continue
    return total


def _format_size(size_bytes: int) -> str:
    units = ("B", "KiB", "MiB", "GiB")
    amount = float(size_bytes)
    for unit in units:
        if amount < 1024 or unit == units[-1]:
            return f"{amount:.1f} {unit}"
        amount /= 1024
    raise AssertionError("unreachable")


if __name__ == "__main__":
    raise SystemExit(main())
