"""Compatibility wrapper for ``sublift-benchmark overhead``."""

from __future__ import annotations

import sys

from sublift.benchmark.cli import main as benchmark_main


def main(argv: list[str] | None = None) -> int:
    """Forward to the unified benchmark CLI using the canonical config."""
    forwarded = list(sys.argv[1:] if argv is None else argv)
    if not forwarded:
        forwarded = [
            "benchmark/configs/zootopia_fixed_region_5fps_perf.json",
            "--label",
            "feat037_overhead",
        ]
    print(
        "提示：scripts/measure_perf_overhead.py 已兼容保留；"
        "新命令请使用 `uv run sublift-benchmark overhead ...`。",
        file=sys.stderr,
    )
    return benchmark_main(["overhead", *forwarded])


if __name__ == "__main__":
    raise SystemExit(main())
