#!/usr/bin/env python3
"""Dump or check frozen changepoint parity golden (feat-06102).

Usage (repo root):
  uv run python scripts/parity/dump_changepoint.py
  uv run python scripts/parity/dump_changepoint.py --check
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

from sublift.config import ChangePointConfig
from sublift.pipeline.changepoint import ChangePointDetector
from sublift.pipeline.signature import FrameSignature


def fixtures_dir(root: Path) -> Path:
    return root / "benchmark" / "parity" / "fixtures" / "changepoint"


def default_golden_path(root: Path) -> Path:
    return root / "benchmark" / "parity" / "goldens" / "events" / "changepoint.v1.json"


def canonical_json(obj: Any) -> str:
    return json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=False)


def config_fingerprint(cfg: dict[str, Any]) -> str:
    return "sha256:" + hashlib.sha256(canonical_json(cfg).encode("utf-8")).hexdigest()


def config_to_dict(cfg: ChangePointConfig) -> dict[str, Any]:
    return {
        "presence_threshold": cfg.presence_threshold,
        "hysteresis_frames": cfg.hysteresis_frames,
        "change_threshold": cfg.change_threshold,
        "enable_ssim_verify": cfg.enable_ssim_verify,
        "ssim_threshold": cfg.ssim_threshold,
        "ssim_window_size": cfg.ssim_window_size,
        "enable_ssim_patrol": cfg.enable_ssim_patrol,
        "ssim_patrol_interval": cfg.ssim_patrol_interval,
        "ssim_patrol_threshold": cfg.ssim_patrol_threshold,
        "ssim_patrol_use_mask": cfg.ssim_patrol_use_mask,
    }


def load_crops(fdir: Path) -> dict[str, np.ndarray]:
    manifest_path = fdir / "crops_manifest.json"
    if not manifest_path.is_file():
        return {}
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    out: dict[str, np.ndarray] = {}
    for c in manifest.get("crops", []):
        path = fdir / c["asset"]
        raw = path.read_bytes()
        w, h = int(c["width"]), int(c["height"])
        expected = w * h * 3
        if len(raw) != expected:
            raise ValueError(
                f"crop {c['name']}: size mismatch expected={expected} got={len(raw)}"
            )
        digest = "sha256:" + hashlib.sha256(raw).hexdigest()
        if digest != c["input_asset_sha256"]:
            raise ValueError(f"crop {c['name']}: sha256 mismatch")
        out[c["name"]] = np.frombuffer(raw, dtype=np.uint8).reshape(h, w, 3).copy()
    return out


def load_scenarios(fdir: Path) -> list[dict[str, Any]]:
    paths = sorted(fdir.glob("*.scenario.json"))
    if not paths:
        raise SystemExit(f"no scenarios under {fdir}; run gen_changepoint_fixtures.py")
    return [json.loads(p.read_text(encoding="utf-8")) for p in paths]


def run_scenario(
    sc: dict[str, Any], crops: dict[str, np.ndarray]
) -> list[dict[str, Any]]:
    cfg = ChangePointConfig(**sc["change_point_config"])
    det = ChangePointDetector(config=cfg)
    events: list[dict[str, Any]] = []
    for fr in sc["frames"]:
        sig = FrameSignature(
            timestamp_ms=int(fr["timestamp_ms"]),
            foreground_ratio=float(fr["foreground_ratio"]),
            dhash=int(fr["dhash"]),
        )
        crop_name = fr.get("crop")
        crop = crops[crop_name] if crop_name else None
        ev = det.process(sig, crop)
        if ev is not None:
            events.append(
                {
                    "event_type": ev.event_type.name,
                    "timestamp_ms": ev.timestamp_ms,
                    "prev_end_ms": ev.prev_end_ms,
                }
            )
    return events


def build_envelope(root: Path) -> dict[str, Any]:
    fdir = fixtures_dir(root)
    scenarios_in = load_scenarios(fdir)
    crops_map = load_crops(fdir)
    manifest_path = fdir / "crops_manifest.json"
    crops_meta = (
        json.loads(manifest_path.read_text(encoding="utf-8")).get("crops", [])
        if manifest_path.is_file()
        else []
    )

    scenarios_out: list[dict[str, Any]] = []
    for sc in scenarios_in:
        events = run_scenario(sc, crops_map)
        scenarios_out.append(
            {
                "name": sc["name"],
                "change_point_config": sc["change_point_config"],
                "frames": sc["frames"],
                "events": events,
            }
        )

    # Fingerprint default ChangePointConfig for oracle metadata.
    default_fp = config_fingerprint(config_to_dict(ChangePointConfig()))
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
        "change_point_config_fingerprint": default_fp,
    }
    return {
        "golden_schema_version": 1,
        "kind": "changepoint",
        "oracle": oracle,
        "crops": crops_meta,
        "scenarios": scenarios_out,
    }


def write_golden(path: Path, envelope: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(envelope, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )


def golden_core(envelope: dict[str, Any]) -> dict[str, Any]:
    return {
        "crops": envelope.get("crops", []),
        "scenarios": [
            {
                "name": s["name"],
                "change_point_config": s["change_point_config"],
                "frames": s["frames"],
                "events": s["events"],
            }
            for s in envelope["scenarios"]
        ],
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
    if disk.get("kind") != "changepoint":
        print("kind must be 'changepoint'", file=sys.stderr)
        return 1
    if golden_core(disk) != golden_core(live):
        print("changepoint golden core differs from live oracle", file=sys.stderr)
        print(f"  disk oracle_commit: {disk.get('oracle', {}).get('oracle_commit')}")
        print(f"  live oracle_commit: {live['oracle']['oracle_commit']}")
        disk_by = {s["name"]: s for s in disk.get("scenarios", [])}
        live_by = {s["name"]: s for s in live["scenarios"]}
        for name in sorted(set(disk_by) | set(live_by)):
            if disk_by.get(name) != live_by.get(name):
                print(f"  scenario {name}:", file=sys.stderr)
                print(f"    disk events: {disk_by.get(name, {}).get('events')}", file=sys.stderr)
                print(f"    live events: {live_by.get(name, {}).get('events')}", file=sys.stderr)
        return 1
    n = len(disk["scenarios"])
    commit = disk.get("oracle", {}).get("oracle_commit")
    print(f"OK changepoint golden matches oracle ({n} scenarios, oracle_commit={commit})")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-o", "--output", type=Path, default=None)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--allow-dirty", action="store_true")
    args = parser.parse_args(argv)
    root = repo_root()
    path = args.output if args.output is not None else default_golden_path(root)
    if args.check:
        return check_golden(path, root)
    if git_dirty(root) and not args.allow_dirty:
        print(
            "refusing to write changepoint golden on a dirty worktree "
            "(use --allow-dirty to override)",
            file=sys.stderr,
        )
        return 1
    envelope = build_envelope(root)
    write_golden(path, envelope)
    print(f"wrote {path}")
    print(f"oracle_commit={envelope['oracle']['oracle_commit']}")
    print(f"git_dirty={envelope['oracle']['git_dirty']}")
    for s in envelope["scenarios"]:
        print(f"  {s['name']}: {s['events']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
