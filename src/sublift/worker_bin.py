"""Locate Native Worker/CLI binaries for isolated offline tools.

Product extraction uses Native `build/cpp/bin/sublift`, not this module.
"""

from __future__ import annotations

import os
import shutil
import subprocess
from collections.abc import Mapping
from pathlib import Path


def _repo_root(repo_root: Path | None) -> Path:
    if repo_root is not None:
        return repo_root
    return Path(__file__).resolve().parents[2]


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

    root = _repo_root(repo_root)

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


def resolve_native_cli(repo_root: Path | None = None) -> Path | None:
    """Locate Native product CLI (`sublift` or `sublift_cli`)."""
    root = _repo_root(repo_root)
    env_path = os.environ.get("SUBLIFT_CLI_PATH")
    if env_path and os.access(env_path, os.X_OK):
        return Path(env_path)
    for rel in (
        Path("build") / "cpp-rel" / "bin" / "sublift",
        Path("build") / "cpp" / "bin" / "sublift",
        Path("build") / "cpp-rel" / "bin" / "sublift_cli",
        Path("build") / "cpp" / "bin" / "sublift_cli",
    ):
        candidate = root / rel
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    found = shutil.which("sublift")
    if found and os.access(found, os.X_OK):
        return Path(found)
    return None


def probe_cpp_paddle_available(
    *,
    env_override: Mapping[str, str] | None = None,
    repo_root: Path | None = None,
) -> bool:
    """True when the Native worker reports paddle capability."""
    env = env_override if env_override is not None else os.environ
    flag = (env.get("SUBLIFT_CPP_PADDLE") or "").strip().lower()
    if flag in {"0", "false", "no", "off"}:
        return False
    worker_bin = resolve_worker_bin(repo_root)
    if worker_bin is None or not worker_bin.is_file():
        return False
    try:
        res = subprocess.run(
            [str(worker_bin), "--probe-engine", "paddle"],
            capture_output=True,
            text=True,
            timeout=2.0,
            env=dict(env),
            check=False,
        )
        return res.returncode == 0
    except Exception:
        return False

