#!/usr/bin/env python3
"""Generate changepoint parity fixtures (feat-06102).

Writes scenario JSON under ``benchmark/parity/fixtures/changepoint/`` and
optional crop RGB assets for the patrol scenario.

Usage (repo root):
  uv run python scripts/parity/gen_changepoint_fixtures.py
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path

import numpy as np

FIXTURE_DIR = (
    Path(__file__).resolve().parents[2]
    / "benchmark"
    / "parity"
    / "fixtures"
    / "changepoint"
)
CROPS_DIR = FIXTURE_DIR / "crops"


def sha256_bytes(raw: bytes) -> str:
    return "sha256:" + hashlib.sha256(raw).hexdigest()


def write_json(path: Path, obj: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(obj, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def text_crop(rect_x: int, w: int = 320, h: int = 80) -> np.ndarray:
    """Mid-gray RGB with a dark rectangle (matches tests/test_changepoint.py idea)."""
    img = np.full((h, w, 3), 180, dtype=np.uint8)
    img[20:60, rect_x : rect_x + 80] = 20
    return img


def write_crop(name: str, pixels: np.ndarray) -> dict:
    CROPS_DIR.mkdir(parents=True, exist_ok=True)
    raw = np.ascontiguousarray(pixels, dtype=np.uint8).tobytes()
    (CROPS_DIR / f"{name}.rgb").write_bytes(raw)
    h, w = pixels.shape[:2]
    return {
        "name": name,
        "asset": f"crops/{name}.rgb",
        "pixel_semantics": "rgb24",
        "width": int(w),
        "height": int(h),
        "input_asset_sha256": sha256_bytes(raw),
    }


def frame(
    ts: int,
    fg: float,
    dhash: int = 0,
    crop: str | None = None,
) -> dict:
    return {
        "timestamp_ms": ts,
        "foreground_ratio": fg,
        "dhash": dhash,
        "crop": crop,
    }


def main() -> int:
    FIXTURE_DIR.mkdir(parents=True, exist_ok=True)
    crops = [
        write_crop("text_a", text_crop(40)),
        write_crop("text_b", text_crop(160)),
    ]

    default_cp = {
        "presence_threshold": 0.01,
        "hysteresis_frames": 1,
        "change_threshold": 10,
        "enable_ssim_verify": False,
        "ssim_threshold": 0.95,
        "ssim_window_size": 7,
        "enable_ssim_patrol": True,
        "ssim_patrol_interval": 3,
        "ssim_patrol_threshold": 0.92,
        "ssim_patrol_use_mask": True,
    }

    scenarios = [
        {
            "name": "in_out_default",
            "change_point_config": dict(default_cp),
            "frames": [
                frame(0, 0.0),
                frame(200, 0.05, dhash=0b10101010),
                frame(400, 0.05, dhash=0b10101010),
                frame(600, 0.0),
            ],
        },
        {
            "name": "change_dhash_default",
            "change_point_config": {
                **default_cp,
                "change_threshold": 5,
            },
            "frames": [
                frame(0, 0.05, dhash=0b10101010),
                frame(200, 0.05, dhash=0b10101010),
                frame(400, 0.05, dhash=0b01010101),
                frame(600, 0.05, dhash=0b01010101),
            ],
        },
        {
            "name": "hysteresis_2_flicker_and_in",
            "change_point_config": {**default_cp, "hysteresis_frames": 2},
            "frames": [
                frame(0, 0.0),
                frame(200, 0.05),  # flash — no IN
                frame(400, 0.0),
                frame(600, 0.05),
                frame(800, 0.05),  # IN @ 600
            ],
        },
        {
            "name": "hysteresis_2_out",
            "change_point_config": {**default_cp, "hysteresis_frames": 2},
            "frames": [
                frame(0, 0.05),
                frame(200, 0.05),
                frame(400, 0.0),
                frame(600, 0.0),  # OUT @ 400
            ],
        },
        {
            "name": "jitter_no_change",
            "change_point_config": {**default_cp, "change_threshold": 5},
            "frames": [
                frame(0, 0.05, dhash=0b10101010),
                frame(200, 0.05, dhash=0b10101010),
                frame(400, 0.05, dhash=0b01010101),
                frame(600, 0.05, dhash=0b10101010),  # back — no CHANGE
            ],
        },
        {
            "name": "stable_persistence",
            "change_point_config": dict(default_cp),
            "frames": [
                frame(0, 0.05, dhash=0b10101010),
                frame(200, 0.05, dhash=0b10101010),
                frame(400, 0.05, dhash=0b10101010),
                frame(600, 0.05, dhash=0b10101010),
            ],
        },
        {
            # dHash distance forced low (threshold 100); structure change via SSIM patrol.
            "name": "patrol_structure_change",
            "change_point_config": {
                **default_cp,
                "change_threshold": 100,
                "ssim_patrol_interval": 1,
                "ssim_patrol_threshold": 0.95,
                "hysteresis_frames": 1,
            },
            "frames": [
                frame(0, 0.05, dhash=1, crop="text_a"),
                frame(200, 0.05, dhash=1, crop="text_a"),
                frame(400, 0.05, dhash=1, crop="text_b"),  # patrol candidate
                frame(600, 0.05, dhash=1, crop="text_b"),  # stable confirm CHANGE
            ],
        },
        {
            # dHash would fire CHANGE, but high SSIM between identical crops vetoes it.
            "name": "ssim_verify_veto",
            "change_point_config": {
                **default_cp,
                "change_threshold": 5,
                "enable_ssim_verify": True,
                "ssim_threshold": 0.90,
                "enable_ssim_patrol": False,
                "hysteresis_frames": 1,
            },
            "frames": [
                frame(0, 0.05, dhash=0b10101010, crop="text_a"),
                frame(200, 0.05, dhash=0b10101010, crop="text_a"),
                # large dHash distance but same crop structure → SSIM veto, no CHANGE
                frame(400, 0.05, dhash=0b01010101, crop="text_a"),
                frame(600, 0.05, dhash=0b01010101, crop="text_a"),
            ],
        },
        {
            # dHash exceeds + low SSIM (different crops) → CHANGE allowed.
            "name": "ssim_verify_allow_change",
            "change_point_config": {
                **default_cp,
                "change_threshold": 5,
                "enable_ssim_verify": True,
                "ssim_threshold": 0.95,
                "enable_ssim_patrol": False,
                "hysteresis_frames": 1,
            },
            "frames": [
                frame(0, 0.05, dhash=0b10101010, crop="text_a"),
                frame(200, 0.05, dhash=0b10101010, crop="text_a"),
                frame(400, 0.05, dhash=0b01010101, crop="text_b"),
                frame(600, 0.05, dhash=0b01010101, crop="text_b"),
            ],
        },
    ]

    for sc in scenarios:
        write_json(FIXTURE_DIR / f"{sc['name']}.scenario.json", sc)

    write_json(FIXTURE_DIR / "crops_manifest.json", {"crops": crops})
    print(f"wrote {len(scenarios)} scenarios + {len(crops)} crops to {FIXTURE_DIR}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
