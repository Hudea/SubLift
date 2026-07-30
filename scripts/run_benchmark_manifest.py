"""Compatibility wrapper for the former benchmark entrypoint.

New commands should use:

    uv run sublift-benchmark run benchmark/configs/<name>.json
"""

from __future__ import annotations

import sys
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parents[1]
if str(_REPO_ROOT / "src") not in sys.path:
    sys.path.insert(0, str(_REPO_ROOT / "src"))

from sublift.benchmark.cli import main as benchmark_main  # noqa: E402


def main(argv: list[str] | None = None) -> int:
    """Forward historical arguments to ``sublift-benchmark run``."""
    forwarded = list(sys.argv[1:] if argv is None else argv)
    if "--label" not in forwarded:
        forwarded.extend(["--label", "auto"])
    print(
        "提示：scripts/run_benchmark_manifest.py 已兼容保留；"
        "新命令请使用 `uv run sublift-benchmark run ...`。",
        file=sys.stderr,
    )
    return benchmark_main(["run", *forwarded])


if __name__ == "__main__":
    raise SystemExit(main())
