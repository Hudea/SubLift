#!/usr/bin/env python3
"""Dump or check frozen default Config golden (feat-06004).

Usage (repo root):
  uv run python scripts/parity/dump_config.py
  uv run python scripts/parity/dump_config.py --check
"""

from __future__ import annotations

import argparse
import hashlib
import json
import platform
import subprocess
import sys
from dataclasses import asdict
from pathlib import Path
from typing import Any


def repo_root() -> Path:
    try:
        out = subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"],
            text=True,
            stderr=subprocess.DEVNULL,
        )
        return Path(out.strip())
    except (subprocess.CalledProcessError, FileNotFoundError):
        return Path(__file__).resolve().parents[2]


def default_golden_path(root: Path) -> Path:
    return root / "benchmark" / "parity" / "goldens" / "config" / "default_config.v1.json"


def config_to_dict() -> dict[str, Any]:
    from sublift.config import DEFAULT_CONFIG

    # asdict already recurses into nested dataclasses (e.g. signature / change_point).
    return asdict(DEFAULT_CONFIG)


def canonical_config_json(config: dict[str, Any]) -> str:
    return json.dumps(config, sort_keys=True, separators=(",", ":"), ensure_ascii=False)


def config_fingerprint(config: dict[str, Any]) -> str:
    digest = hashlib.sha256(canonical_config_json(config).encode("utf-8")).hexdigest()
    return f"sha256:{digest}"


def git_commit(root: Path) -> str:
    return subprocess.check_output(
        ["git", "rev-parse", "HEAD"],
        cwd=root,
        text=True,
    ).strip()


def git_branch(root: Path) -> str:
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "--abbrev-ref", "HEAD"],
            cwd=root,
            text=True,
        ).strip()
    except subprocess.CalledProcessError:
        return "unknown"


def git_dirty(root: Path) -> bool:
    try:
        out = subprocess.check_output(
            ["git", "status", "--porcelain"],
            cwd=root,
            text=True,
        )
        return bool(out.strip())
    except subprocess.CalledProcessError:
        return False


def optional_version(module: str, attr: str = "__version__") -> str | None:
    try:
        mod = __import__(module)
        value = getattr(mod, attr, None)
        return str(value) if value is not None else None
    except Exception:
        return None


def ffmpeg_version_line() -> str | None:
    try:
        out = subprocess.check_output(
            ["ffmpeg", "-version"],
            text=True,
            stderr=subprocess.STDOUT,
        )
        return out.splitlines()[0] if out else None
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None


def build_envelope(root: Path) -> dict[str, Any]:
    config = config_to_dict()
    dirty = git_dirty(root)
    if dirty:
        print("warning: working tree is dirty; oracle_commit still records HEAD", file=sys.stderr)

    macos = platform.mac_ver()[0] or None
    oracle: dict[str, Any] = {
        "oracle_commit": git_commit(root),
        "oracle_branch": git_branch(root),
        "python_version": platform.python_version(),
        "opencv_version": optional_version("cv2"),
        "numpy_version": optional_version("numpy"),
        "ffmpeg_version": ffmpeg_version_line(),
        "macos_version": macos if platform.system() == "Darwin" else None,
        "vision_note": None,
        "input_asset_sha256": None,
        "config_fingerprint": config_fingerprint(config),
        "git_dirty": dirty,
    }
    return {
        "golden_schema_version": 1,
        "kind": "config",
        "oracle": oracle,
        "config": config,
    }


def write_golden(path: Path, envelope: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(envelope, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )


def check_golden(path: Path, root: Path) -> int:
    if not path.is_file():
        print(f"missing golden: {path}", file=sys.stderr)
        return 1
    disk = json.loads(path.read_text(encoding="utf-8"))
    live = build_envelope(root)
    if disk.get("golden_schema_version") != 1:
        print("golden_schema_version must be 1", file=sys.stderr)
        return 1
    if disk.get("kind") != "config":
        print("kind must be 'config'", file=sys.stderr)
        return 1
    if disk.get("config") != live["config"]:
        print("config body differs from DEFAULT_CONFIG", file=sys.stderr)
        print(f"  disk oracle_commit: {disk.get('oracle', {}).get('oracle_commit')}")
        print(f"  live oracle_commit: {live['oracle']['oracle_commit']}")
        return 1
    disk_fp = disk.get("oracle", {}).get("config_fingerprint")
    live_fp = live["oracle"]["config_fingerprint"]
    if disk_fp != live_fp:
        print("config_fingerprint mismatch", file=sys.stderr)
        print(f"  disk: {disk_fp}")
        print(f"  live: {live_fp}")
        return 1
    print(
        "OK config golden matches DEFAULT_CONFIG "
        f"(oracle_commit={disk.get('oracle', {}).get('oracle_commit')})"
    )
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=None,
        help="Golden path (default: benchmark/parity/goldens/config/default_config.v1.json)",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="Verify committed golden against DEFAULT_CONFIG (no write)",
    )
    args = parser.parse_args(argv)
    root = repo_root()
    path = args.output if args.output is not None else default_golden_path(root)
    if args.check:
        return check_golden(path, root)
    envelope = build_envelope(root)
    write_golden(path, envelope)
    print(f"wrote {path}")
    print(f"oracle_commit={envelope['oracle']['oracle_commit']}")
    print(f"config_fingerprint={envelope['oracle']['config_fingerprint']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
