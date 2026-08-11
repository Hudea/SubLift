"""Historical benchmark wrappers must only forward the canonical CLI."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize(
    ("script", "command"),
    [
        ("scripts/run_benchmark_manifest.py", "run"),
        ("scripts/measure_perf_overhead.py", "overhead"),
        ("scripts/compare_roi_ab.py", "compare-roi"),
    ],
)
def test_wrapper_forwards_help_to_canonical_cli(script: str, command: str) -> None:
    completed = subprocess.run(
        [sys.executable, str(_REPO_ROOT / script), "--help"],
        cwd=_REPO_ROOT,
        capture_output=True,
        check=False,
        text=True,
    )

    assert completed.returncode == 0, completed.stderr
    assert f"sublift-benchmark {command}" in completed.stderr
    assert "usage:" in completed.stdout
