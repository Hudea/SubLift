#!/usr/bin/env python3
"""Dump or verify Apple Vision OCR geometry & parity golden (feat-06405).

Usage:
  python scripts/parity/dump_vision.py [--output PATH] [--check] [--allow-dirty] [--live]

Schema:
  golden_schema_version: 1
  kind: "vision"

L0 (init hard gate): box mapping, clamp, line sort, empty OcrResult structure.
L4 (optional, not init hard gate): live Vision text/conf — `--live` is reserved;
  currently only annotates vision_note (does not run Vision or dump OCR text).
"""

from __future__ import annotations

import argparse
import json
import platform
import subprocess
import sys
from pathlib import Path
from typing import Any

# Add repo root to path
REPO_ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "src"))

from sublift.models import BoundingBox, OcrLine  # noqa: E402
from sublift.ocr.vision import (  # noqa: E402
    DEFAULT_RECOGNITION_LANGUAGES,
    _clamp_box,
    _vision_box_to_pixel,
    is_vision_available,
)

GOLDEN_VERSION = 1
DEFAULT_GOLDEN_PATH = (
    REPO_ROOT / "benchmark" / "parity" / "goldens" / "vision" / "vision.v1.json"
)

# Envelope keys that must be present in frozen golden (values not L0-compared).
REQUIRED_ROOT_KEYS = (
    "golden_schema_version",
    "kind",
    "oracle",
    "default_recognition_languages",
    "cases",
)
REQUIRED_ORACLE_KEYS = (
    "oracle_commit",
    "macos_version",
    "vision_note",
)


def get_git_info() -> tuple[str, str, bool]:
    """Get current git commit, branch, and dirty status."""
    try:
        commit = subprocess.check_output(
            ["git", "rev-parse", "HEAD"], text=True
        ).strip()
        branch = subprocess.check_output(
            ["git", "rev-parse", "--abbrev-ref", "HEAD"], text=True
        ).strip()
        status = subprocess.check_output(
            ["git", "status", "--porcelain"], text=True
        ).strip()
        dirty = bool(status)
        return commit, branch, dirty
    except Exception:
        return "unknown", "unknown", False


def build_vision_golden_envelope(
    allow_dirty: bool = False, live: bool = False
) -> dict[str, Any]:
    """Build the canonical Vision parity golden envelope."""
    commit, branch, dirty = get_git_info()
    if dirty and not allow_dirty:
        print(
            "Warning: git status is dirty. Pass --allow-dirty to build golden anyway.",
            file=sys.stderr,
        )

    # --live is reserved for optional L4 text/conf recording (not implemented).
    # Annotate vision_note only; do not run Apple Vision or dump OCR text here.
    vision_note = "Apple Vision PyObjC oracle"
    if live:
        vision_note += (
            " (--live reserved: L4 live OCR recording not implemented; "
            "no Vision run / no text/conf sidecar)"
        )

    oracle_meta = {
        "oracle_commit": commit,
        "oracle_branch": branch,
        "python_version": sys.version.split()[0],
        "git_dirty": dirty,
        "macos_version": (
            platform.mac_ver()[0] if sys.platform == "darwin" else "non-macos"
        ),
        "vision_available": is_vision_available(),
        "vision_note": vision_note,
    }

    cases: list[dict[str, Any]] = []

    # 1. normalized_box_to_pixel test cases
    box_cases = [
        ("standard_center_box", 0.1, 0.2, 0.5, 0.3, 100, 100),
        ("bankers_round_half_even_1.5", 0.015, 0.0, 0.1, 0.1, 100, 100),
        ("bankers_round_half_even_2.5", 0.025, 0.0, 0.1, 0.1, 100, 100),
        ("clamp_overflow_right_bottom", -0.1, -0.1, 1.5, 1.5, 100, 100),
        ("zero_image_dimensions", 0.1, 0.2, 0.5, 0.3, 0, 0),
        ("zero_box_dimensions", 0.5, 0.5, 0.0, 0.0, 200, 100),
    ]

    for name, nx, ny, nw, nh, img_w, img_h in box_cases:
        res = _vision_box_to_pixel((nx, ny, nw, nh), img_w, img_h)
        cases.append({
            "name": name,
            "kind": "normalized_box_to_pixel",
            "nx": nx,
            "ny": ny,
            "nw": nw,
            "nh": nh,
            "image_width": img_w,
            "image_height": img_h,
            "expected_box": {
                "x": res.x,
                "y": res.y,
                "width": res.width,
                "height": res.height,
            },
        })

    # 2. clamp_box test cases
    clamp_cases = [
        ("clamp_within_bounds", 10, 20, 30, 40, 100, 100),
        ("clamp_negative_origin", -10, -5, 50, 50, 100, 100),
        ("clamp_exceed_width_height", 80, 80, 50, 50, 100, 100),
        ("clamp_zero_img_bounds", 10, 10, 20, 20, 0, 0),
    ]

    for name, x, y, w, h, img_w, img_h in clamp_cases:
        res = _clamp_box(x, y, w, h, img_w, img_h)
        cases.append({
            "name": name,
            "kind": "clamp_box",
            "x": x,
            "y": y,
            "w": w,
            "h": h,
            "image_width": img_w,
            "image_height": img_h,
            "expected_box": {
                "x": res.x,
                "y": res.y,
                "width": res.width,
                "height": res.height,
            },
        })

    # 3. line_sorting test cases
    raw_lines = [
        OcrLine(
            text="Line2",
            confidence=0.9,
            box=BoundingBox(x=10, y=50, width=100, height=20),
        ),
        OcrLine(
            text="Line1_Right",
            confidence=0.95,
            box=BoundingBox(x=80, y=10, width=50, height=20),
        ),
        OcrLine(
            text="Line1_Left",
            confidence=0.88,
            box=BoundingBox(x=20, y=10, width=50, height=20),
        ),
    ]
    lines_copy = list(raw_lines)
    lines_copy.sort(key=lambda line: (line.box.y, line.box.x))

    cases.append({
        "name": "sort_multiline_y_then_x",
        "kind": "line_sorting",
        "input_lines": [
            {
                "text": line.text,
                "confidence": line.confidence,
                "box": {
                    "x": line.box.x,
                    "y": line.box.y,
                    "width": line.box.width,
                    "height": line.box.height,
                },
            }
            for line in raw_lines
        ],
        "expected_lines": [
            {
                "text": line.text,
                "confidence": line.confidence,
                "box": {
                    "x": line.box.x,
                    "y": line.box.y,
                    "width": line.box.width,
                    "height": line.box.height,
                },
            }
            for line in lines_copy
        ],
    })

    # 4. empty OcrResult structure (L0) — perform-fail / no-obs / all-blank
    cases.append({
        "name": "empty_result_default_structure",
        "kind": "empty_result",
        "expected": {
            "text": "",
            "confidence": 0.0,
            "lines": [],
        },
    })

    return {
        "golden_schema_version": GOLDEN_VERSION,
        "kind": "vision",
        "oracle": oracle_meta,
        "default_recognition_languages": list(DEFAULT_RECOGNITION_LANGUAGES),
        "cases": cases,
    }


def _check_envelope(existing: dict[str, Any]) -> list[str]:
    """Validate schema/kind/oracle envelope keys; return list of failure messages."""
    failures: list[str] = []
    for key in REQUIRED_ROOT_KEYS:
        if key not in existing:
            failures.append(f"missing root key: {key}")
    if existing.get("kind") != "vision":
        failures.append(f"kind must be 'vision', got {existing.get('kind')!r}")
    if existing.get("golden_schema_version") != GOLDEN_VERSION:
        failures.append(
            f"golden_schema_version must be {GOLDEN_VERSION}, "
            f"got {existing.get('golden_schema_version')!r}"
        )
    oracle = existing.get("oracle")
    if not isinstance(oracle, dict):
        failures.append("oracle must be an object")
    else:
        for key in REQUIRED_ORACLE_KEYS:
            if key not in oracle:
                failures.append(f"missing oracle key: {key}")
            elif oracle.get(key) in (None, ""):
                failures.append(f"oracle.{key} must be non-empty")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Dump or check Vision parity golden (L0 geometry + empty structure)."
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=DEFAULT_GOLDEN_PATH,
        help="Path to golden JSON file",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="Check existing golden file against Python oracle (L0 cases + envelope)",
    )
    parser.add_argument(
        "--allow-dirty",
        action="store_true",
        help="Allow building golden when git is dirty",
    )
    parser.add_argument(
        "--live",
        action="store_true",
        help=(
            "Reserved for optional L4 live Vision text/conf recording. "
            "Currently a no-op besides vision_note annotation; does not run Vision."
        ),
    )

    args = parser.parse_args()

    golden_data = build_vision_golden_envelope(
        allow_dirty=args.allow_dirty, live=args.live
    )
    golden_json = json.dumps(golden_data, indent=2, ensure_ascii=False) + "\n"

    if args.check:
        if not args.output.exists():
            print(
                f"[FAIL] Vision golden file does not exist: {args.output}",
                file=sys.stderr,
            )
            return 1
        try:
            with open(args.output, encoding="utf-8") as f:
                existing_data = json.load(f)
        except json.JSONDecodeError as exc:
            print(
                f"[FAIL] Invalid JSON in {args.output}: {exc}", file=sys.stderr
            )
            return 1

        mismatch = False
        for msg in _check_envelope(existing_data):
            print(f"[FAIL] envelope: {msg}", file=sys.stderr)
            mismatch = True

        if (
            existing_data.get("default_recognition_languages")
            != golden_data["default_recognition_languages"]
        ):
            print("[FAIL] default_recognition_languages mismatch!", file=sys.stderr)
            mismatch = True
        if existing_data.get("cases") != golden_data["cases"]:
            print("[FAIL] cases mismatch!", file=sys.stderr)
            mismatch = True

        if mismatch:
            return 1
        print(f"[OK] Vision golden check passed: {args.output}")
        return 0

    # Dump mode
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with open(args.output, "w", encoding="utf-8") as f:
        f.write(golden_json)

    print(f"Successfully dumped Vision golden to {args.output}")
    if args.live:
        print(
            "Note: --live is reserved (L4); no Vision OCR text/conf was recorded.",
            file=sys.stderr,
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
