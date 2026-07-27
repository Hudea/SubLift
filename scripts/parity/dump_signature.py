#!/usr/bin/env python3
"""Dump or check frozen signature parity golden (feat-06101).

Runs the Python signature oracle over the deterministic fixtures produced by
``gen_signature_fixtures.py`` and writes a frozen golden envelope. The C++
candidate loads the same golden and compares (L0 dhash/timestamp_ms exact,
L1 fg_ratio epsilon) - see docs/cpp/parity-contract.md §4/§9.

Reuses oracle-metadata helpers from dump_config.py (git/version probes).

Usage (repo root):
  uv run python scripts/parity/dump_signature.py
  uv run python scripts/parity/dump_signature.py --check
"""

from __future__ import annotations

import argparse
import hashlib
import json
import platform
import sys
from pathlib import Path
from typing import Any

import numpy as np
from dump_config import (
    ffmpeg_version_line,
    git_branch,
    git_commit,
    git_dirty,
    optional_version,
    repo_root,
)

from sublift.config import SignatureConfig
from sublift.pipeline.signature import compute_signature


def fixtures_dir(root: Path) -> Path:
    return root / "benchmark" / "parity" / "fixtures" / "signature"


def default_golden_path(root: Path) -> Path:
    return root / "benchmark" / "parity" / "goldens" / "signature" / "signature.v1.json"


def canonical_json(obj: Any) -> str:
    return json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=False)


def sha256_bytes(raw: bytes) -> str:
    return "sha256:" + hashlib.sha256(raw).hexdigest()


def config_fingerprint(cfg: dict[str, Any]) -> str:
    return "sha256:" + hashlib.sha256(canonical_json(cfg).encode("utf-8")).hexdigest()


def load_fixture_image(meta: dict[str, Any], fdir: Path) -> np.ndarray:
    sem = meta["pixel_semantics"]
    w, h = int(meta["width"]), int(meta["height"])
    if w <= 0 or h <= 0:
        raise ValueError(f"fixture {meta.get('name')}: width/height must be > 0")
    channels = 1 if sem == "gray8" else 3
    expected = w * h * channels
    path = fdir / f"{meta['name']}.rgb"
    raw = path.read_bytes()
    if len(raw) != expected:
        raise ValueError(
            f"fixture {meta.get('name')}: size mismatch path={path} "
            f"expected={expected} got={len(raw)}"
        )
    if sem == "gray8":
        return np.frombuffer(raw, dtype=np.uint8).reshape(h, w)
    # rgb24 and bgr24_as_rgb_gray_quirk both read as a 3-channel array; the
    # quirk is reproduced by feeding BGR bytes through COLOR_RGB2GRAY, which
    # happens inside compute_signature -> _to_gray. No channel swap here.
    return np.frombuffer(raw, dtype=np.uint8).reshape(h, w, 3)


def config_to_dict(cfg: SignatureConfig) -> dict[str, Any]:
    return {
        "block_size_ratio": cfg.block_size_ratio,
        "adaptive_c": cfg.adaptive_c,
        "hash_size": cfg.hash_size,
    }


def build_envelope(root: Path) -> dict[str, Any]:
    fdir = fixtures_dir(root)
    metas = sorted(
        (json.loads(p.read_text(encoding="utf-8")) for p in sorted(fdir.glob("*.meta.json"))),
        key=lambda m: m["timestamp_ms"],
    )
    if not metas:
        raise SystemExit(f"no fixture meta found under {fdir}; run gen_signature_fixtures.py")

    cfg = SignatureConfig()
    cfg_dict = config_to_dict(cfg)

    frames = []
    for m in metas:
        img = load_fixture_image(m, fdir)
        sig = compute_signature(img, m["timestamp_ms"], cfg)
        asset = fdir / f"{m['name']}.rgb"
        frames.append(
            {
                "name": m["name"],
                "pixel_semantics": m["pixel_semantics"],
                "asset": f"{m['name']}.rgb",
                "input_asset_sha256": sha256_bytes(asset.read_bytes()),
                "width": m["width"],
                "height": m["height"],
                "timestamp_ms": sig.timestamp_ms,
                "fg_ratio": sig.foreground_ratio,
                "dhash": sig.dhash,
            }
        )

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
        "git_dirty": git_dirty(root),
        "signature_config_fingerprint": config_fingerprint(cfg_dict),
    }
    return {
        "golden_schema_version": 1,
        "kind": "signature",
        "oracle": oracle,
        "signature_config": cfg_dict,
        "fixtures": frames,
    }


def write_golden(path: Path, envelope: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(envelope, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )


_FIXTURE_CORE_KEYS = (
    "name",
    "pixel_semantics",
    "asset",
    "input_asset_sha256",
    "width",
    "height",
    "timestamp_ms",
    "fg_ratio",
    "dhash",
)


def golden_core(envelope: dict[str, Any]) -> dict[str, Any]:
    """Stable core compared by --check (oracle env metadata may drift).

    Includes asset integrity fields (sha/semantics/geometry) so --check catches
    hash-field or metadata drift that leaves dhash/fg_ratio unchanged.
    """
    return {
        "signature_config": envelope["signature_config"],
        "fixtures": [{k: f[k] for k in _FIXTURE_CORE_KEYS} for f in envelope["fixtures"]],
    }


def check_golden(path: Path, root: Path) -> int:
    if not path.is_file():
        print(f"missing golden: {path}", file=sys.stderr)
        return 1
    disk = json.loads(path.read_text(encoding="utf-8"))
    live = build_envelope(root)
    if disk.get("golden_schema_version") != 1:
        print("golden_schema_version must be 1", file=sys.stderr)
        return 1
    if disk.get("kind") != "signature":
        print("kind must be 'signature'", file=sys.stderr)
        return 1
    if golden_core(disk) != golden_core(live):
        print("signature golden core differs from live oracle", file=sys.stderr)
        print(f"  disk oracle_commit: {disk.get('oracle', {}).get('oracle_commit')}")
        print(f"  live oracle_commit: {live['oracle']['oracle_commit']}")
        for disk_f, live_f in zip(disk["fixtures"], live["fixtures"], strict=False):
            if {k: disk_f.get(k) for k in _FIXTURE_CORE_KEYS} != {
                k: live_f.get(k) for k in _FIXTURE_CORE_KEYS
            }:
                print(f"  fixture diff: disk={disk_f} live={live_f}", file=sys.stderr)
        return 1
    n = len(disk["fixtures"])
    commit = disk.get("oracle", {}).get("oracle_commit")
    print(f"OK signature golden matches oracle ({n} fixtures, oracle_commit={commit})")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=None,
        help="Golden path (default: benchmark/parity/goldens/signature/signature.v1.json)",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="Verify committed golden against live oracle (no write)",
    )
    parser.add_argument(
        "--allow-dirty",
        action="store_true",
        help="Allow writing golden while the git worktree is dirty",
    )
    args = parser.parse_args(argv)
    root = repo_root()
    path = args.output if args.output is not None else default_golden_path(root)
    if args.check:
        return check_golden(path, root)
    if git_dirty(root) and not args.allow_dirty:
        print(
            "refusing to write signature golden on a dirty worktree "
            "(use --allow-dirty to override)",
            file=sys.stderr,
        )
        return 1
    envelope = build_envelope(root)
    write_golden(path, envelope)
    print(f"wrote {path}")
    print(f"oracle_commit={envelope['oracle']['oracle_commit']}")
    print(f"git_dirty={envelope['oracle']['git_dirty']}")
    print(f"fixtures={len(envelope['fixtures'])}")
    for f in envelope["fixtures"]:
        ts = f["timestamp_ms"]
        fg = f["fg_ratio"]
        dh = f["dhash"]
        print(f"  {f['name']}: ts={ts} fg={fg:.9f} dhash={dh:#018x}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
