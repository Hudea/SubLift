"""Run a SubLift benchmark from a JSON manifest.

Usage:
    uv run python scripts/run_benchmark_manifest.py benchmark/manifests/example.json
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parents[1]
if str(_REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(_REPO_ROOT))

from benchmark.manifest import ManifestError, load_run_config  # noqa: E402
from benchmark.report import write_reports  # noqa: E402
from benchmark.runner import run_benchmark  # noqa: E402


def build_parser() -> argparse.ArgumentParser:
    """Build the script argument parser."""
    parser = argparse.ArgumentParser(
        description="Run benchmark from a JSON manifest file.",
    )
    parser.add_argument("manifest", type=Path, help="Benchmark manifest JSON path")
    return parser


def main(argv: list[str] | None = None) -> int:
    """Run benchmark and write JSON/CSV/Markdown reports."""
    parser = build_parser()
    args = parser.parse_args(argv)

    try:
        config = load_run_config(args.manifest)
        result = run_benchmark(config)
        paths = write_reports(result)
    except (FileNotFoundError, ManifestError, RuntimeError, ValueError) as exc:
        print(f"benchmark 失败: {exc}", file=sys.stderr)
        return 1

    print("benchmark 完成:")
    for kind, path in paths.items():
        print(f"  {kind}: {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
