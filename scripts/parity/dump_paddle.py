"""
scripts/parity/dump_paddle.py
-----------------------------
Dump Python PaddleOcrEngine Oracle output on synthetic geometry fixtures to golden JSON.
Used for C++ candidate parity verification.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

from sublift.models import BoundingBox, OcrLine, OcrResult
from sublift.ocr.paddle import _VALID_MODEL_TYPES


def generate_paddle_pure_golden() -> dict[str, Any]:
    """Generate pure geometry and sorting golden entries for PaddleOCR parity."""
    # Synthetic test inputs
    raw_lines = [
        OcrLine(
            text="  line2_bottom  ",
            confidence=0.88,
            box=BoundingBox(x=12, y=60, width=120, height=30),
        ),
        OcrLine(
            text="line1_right",
            confidence=0.95,
            box=BoundingBox(x=60, y=15, width=80, height=25),
        ),
        OcrLine(
            text="line1_left",
            confidence=0.91,
            box=BoundingBox(x=10, y=15, width=45, height=25),
        ),
        OcrLine(
            text="   \t\n",
            confidence=0.50,
            box=BoundingBox(x=0, y=0, width=10, height=10),
        ),
    ]

    # Filter empty strip lines (mimics paddle.py)
    filtered = [line for line in raw_lines if line.text.strip()]

    # Sort (y, x) (mimics paddle.py)
    filtered.sort(key=lambda line: (line.box.y, line.box.x))

    res = OcrResult.from_lines(filtered)

    return {
        "version": "1.0",
        "valid_model_types": sorted(_VALID_MODEL_TYPES),
        "text": res.text,
        "confidence": res.confidence,
        "lines": [
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
            for line in res.lines
        ],
    }


def main() -> None:
    parser = argparse.ArgumentParser(description="Dump or check Paddle OCR golden fixture.")
    parser.add_argument(
        "--check", action="store_true", help="Check existing golden file without overwriting."
    )
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parents[2]
    golden_path = repo_root / "benchmark" / "parity" / "goldens" / "paddle" / "paddle.v1.json"

    data = generate_paddle_pure_golden()
    formatted = json.dumps(data, indent=2, ensure_ascii=False) + "\n"

    if args.check:
        if not golden_path.exists():
            print(f"[FAIL] Golden file {golden_path} does not exist", file=sys.stderr)
            sys.exit(1)
        existing = golden_path.read_text(encoding="utf-8")
        if existing.strip() != formatted.strip():
            print(
                f"[FAIL] Golden file {golden_path} does not match generated output",
                file=sys.stderr,
            )
            sys.exit(1)
        print(f"[OK] Golden file {golden_path} matches generated output")
        sys.exit(0)

    golden_path.parent.mkdir(parents=True, exist_ok=True)
    golden_path.write_text(formatted, encoding="utf-8")
    print(f"[OK] Dumped Paddle pure golden to {golden_path}")


if __name__ == "__main__":
    main()
