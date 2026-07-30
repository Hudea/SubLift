"""Compatibility wrapper for ``sublift-benchmark compare-roi``."""

from __future__ import annotations

import sys

from sublift.benchmark.cli import main as benchmark_main


def main(argv: list[str] | None = None) -> int:
    """Forward historical arguments to the unified benchmark CLI."""
    forwarded = list(sys.argv[1:] if argv is None else argv)
    print(
        "提示：scripts/compare_roi_ab.py 已兼容保留；"
        "新命令请使用 `uv run sublift-benchmark compare-roi ...`。",
        file=sys.stderr,
    )
    return benchmark_main(["compare-roi", *forwarded])


if __name__ == "__main__":
    raise SystemExit(main())
