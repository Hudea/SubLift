#!/usr/bin/env python3
"""Generate deterministic signature parity fixtures (feat-06101).

Writes raw RGB24 / Gray8 byte blobs + ``*.meta.json`` under
``benchmark/parity/fixtures/signature/``. Deterministic (no RNG): Python dump
and C++ candidate consume the identical byte sequence, which is what makes
bit-exact dHash parity achievable (no PNG/JPEG decoder in the loop).

Re-run after editing fixture shapes; outputs are committed to the repo.

Usage (repo root):
  uv run python scripts/parity/gen_signature_fixtures.py
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path

import cv2
import numpy as np

from sublift.config import SignatureConfig
from sublift.pipeline.signature import compute_signature

FIXTURE_DIR = (
    Path(__file__).resolve().parents[2] / "benchmark" / "parity" / "fixtures" / "signature"
)

DEFAULT_CONFIG = {"block_size_ratio": 0.08, "adaptive_c": 12, "hash_size": 8}

W, H = 128, 64
BG = 40  # dark gray background


def sha256_bytes(raw: bytes) -> str:
    return "sha256:" + hashlib.sha256(raw).hexdigest()


def write_fixture(
    name: str,
    pixels: np.ndarray,
    semantics: str,
    timestamp_ms: int,
) -> dict:
    FIXTURE_DIR.mkdir(parents=True, exist_ok=True)
    raw = np.ascontiguousarray(pixels, dtype=np.uint8).tobytes()
    (FIXTURE_DIR / f"{name}.rgb").write_bytes(raw)
    h, w = pixels.shape[:2]
    meta = {
        "name": name,
        "pixel_semantics": semantics,
        "width": int(w),
        "height": int(h),
        "timestamp_ms": timestamp_ms,
        "config": dict(DEFAULT_CONFIG),
    }
    (FIXTURE_DIR / f"{name}.meta.json").write_text(
        json.dumps(meta, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    return {"name": name, "input_asset_sha256": sha256_bytes(raw)}


def make_rgb(bg: int = BG) -> np.ndarray:
    return np.full((H, W, 3), bg, dtype=np.uint8)


def stamp_block(img: np.ndarray, x0: int, x1: int, y0: int, y1: int, color: tuple) -> None:
    img[y0:y1, x0:x1] = color


def main() -> int:
    summary: list[dict] = []

    # 1. empty: uniform background -> fg_ratio ~ 0, dhash 0 (baseline / EMPTY path)
    summary.append(write_fixture("empty", make_rgb(), "rgb24", timestamp_ms=0))

    # 2. subtitle: bright subtitle band block -> non-zero fg_ratio, non-zero dhash
    sub = make_rgb()
    stamp_block(sub, 48, 80, 44, 60, (240, 240, 240))
    summary.append(write_fixture("subtitle", sub, "rgb24", timestamp_ms=200))

    # 3. subtitle_b: different block shape/brightness vs subtitle -> distinct dhash
    sub_b = make_rgb()
    stamp_block(sub_b, 40, 88, 46, 58, (230, 230, 230))
    summary.append(write_fixture("subtitle_b", sub_b, "rgb24", timestamp_ms=400))

    # 4. bgr_quirk: yellow block (255,255,0). RGB2GRAY gray≈226 → non-zero
    #    fg/dhash under adaptive threshold; BGR2GRAY gray≈179 often yields
    #    empty mask on this bg — so "fixing" the quirk to BGR2GRAY fails
    #    parity. Generation asserts channel-swap path diverges.
    quirk = make_rgb()
    stamp_block(quirk, 60, 92, 44, 60, (255, 255, 0))
    gray_quirk = cv2.cvtColor(quirk, cv2.COLOR_RGB2GRAY)
    gray_bgr = cv2.cvtColor(quirk, cv2.COLOR_BGR2GRAY)
    if np.array_equal(gray_quirk, gray_bgr):
        raise SystemExit("bgr_quirk fixture is not a trap: RGB2GRAY == BGR2GRAY (redesign color)")
    sig_q = compute_signature(quirk, 600, SignatureConfig())
    # Channel swap + RGB2GRAY ≈ true BGR2GRAY on original channel order.
    bgr_as_rgb = quirk[:, :, ::-1].copy()
    sig_fixed = compute_signature(bgr_as_rgb, 600, SignatureConfig())
    if sig_q.dhash == sig_fixed.dhash and sig_q.foreground_ratio == sig_fixed.foreground_ratio:
        raise SystemExit("bgr_quirk fixture is not a trap: signature identical under channel swap")
    summary.append(write_fixture("bgr_quirk", quirk, "bgr24_as_rgb_gray_quirk", timestamp_ms=600))

    # 5. gray8: 2D grayscale input -> exercises _to_gray 2D path (returns as-is)
    gray = np.full((H, W), BG, dtype=np.uint8)
    gray[44:60, 48:80] = 240
    summary.append(write_fixture("gray8", gray, "gray8", timestamp_ms=800))

    print(f"wrote {len(summary)} fixtures to {FIXTURE_DIR}")
    for s in summary:
        print(f"  {s['name']}: {s['input_asset_sha256']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
