"""Git utilities and auto-increment label resolution for benchmarks."""

from __future__ import annotations

import re
import subprocess
from pathlib import Path


def get_latest_commit_message() -> str:
    """Fetch the first line of the latest git commit message.

    Returns:
        The commit message summary, or an empty string on failure.
    """
    try:
        # Run git log to get the subject of the HEAD commit
        result = subprocess.run(
            ["git", "log", "-1", "--format=%s"],
            capture_output=True,
            text=True,
            check=True,
        )
        return result.stdout.strip()
    except (subprocess.SubprocessError, FileNotFoundError):
        return ""


def clean_commit_message(msg: str) -> str:
    """Clean and extract a 6-character prefix from a commit message.

    Filters common prefixes like 'feat(ocr):', keeps only alphanumeric and
    Chinese characters, and slices the first 6 characters.

    Args:
        msg: The raw commit message subject.

    Returns:
        A cleaned 6-character string, or 'run' if empty/invalid.
    """
    if not msg:
        return "run"

    # Remove typical git commit prefixes: e.g., "feat(ocr): " or "fix: "
    # Pattern matches: word(optional_scope): optional_spaces
    cleaned = re.sub(r"^[a-zA-Z0-9_\-]+(?:\([^)]+\))?\s*:\s*", "", msg)

    # Keep only alphanumeric and Chinese characters, remove spaces and symbols
    cleaned = re.sub(r"[^\w\u4e00-\u9fa5]", "", cleaned)

    if not cleaned:
        return "run"

    # Slice the first 6 characters
    return cleaned[:6]


def resolve_auto_increment_label(
    output_dir: Path,
    base_label: str,
) -> tuple[str, Path]:
    """Scan output_dir and find the next auto-increment sequence index.

    E.g. if directories like '字幕层筛选_1' and '字幕层筛选_2' exist,
    returns ('字幕层筛选_3', output_dir / '字幕层筛选_3').

    Args:
        output_dir: The base benchmark reports directory.
        base_label: The cleaned 6-character commit label.

    Returns:
        A tuple of (final_label, final_output_dir).
    """
    existing_indices = []

    if output_dir.exists():
        for path in output_dir.iterdir():
            name = path.name
            prefix = f"{base_label}_"
            if name.startswith(prefix):
                suffix = name[len(prefix) :]
                match = re.match(r"^(\d+)", suffix)
                if match:
                    existing_indices.append(int(match.group(1)))

    next_idx = max(existing_indices) + 1 if existing_indices else 1
    final_label = f"{base_label}_{next_idx}"
    return final_label, output_dir / final_label
