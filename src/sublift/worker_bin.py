"""Resolve sublift_worker executable path (shared by CLI and tools)."""

from __future__ import annotations

import os
import shutil
from pathlib import Path


def resolve_worker_bin(repo_root: Path | None = None) -> Path | None:
    """Locate an executable sublift_worker.

    Priority:
      1. SUBLIFT_WORKER_PATH
      2. {repo}/build/cpp-rel/bin/sublift_worker  (Release)
      3. {repo}/build/cpp/bin/sublift_worker      (Debug)
      4. PATH via shutil.which
    """
    env_path = os.environ.get("SUBLIFT_WORKER_PATH")
    if env_path and os.access(env_path, os.X_OK):
        return Path(env_path)

    root = repo_root
    if root is None:
        # src/sublift/worker_bin.py → parents[2] == repo root
        root = Path(__file__).resolve().parents[2]

    for rel in (
        Path("build") / "cpp-rel" / "bin" / "sublift_worker",
        Path("build") / "cpp" / "bin" / "sublift_worker",
    ):
        candidate = root / rel
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate

    system_which = shutil.which("sublift_worker")
    if system_which and os.access(system_which, os.X_OK):
        return Path(system_which)

    return None
