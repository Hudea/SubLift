#!/usr/bin/env python3
"""Generate deterministic, local-only Paddle Q2 quality clips.

The generated MP4 files live under ``debug/paddle-quality`` and are never
treated as source artifacts. The committed ASS recipes and SRT ground truth
are the versioned contract; live reports fingerprint the actual generated
videos, FFmpeg build, and font assets.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[2]
FIXTURE_DIR = REPO_ROOT / "benchmark" / "fixtures" / "paddle_quality"
DEFAULT_OUTPUT_DIR = REPO_ROOT / "debug" / "paddle-quality"
DURATION_SECONDS = 180.0

RECIPES: tuple[dict[str, str], ...] = (
    {
        "id": "synthetic_latin_motion",
        "ass": "latin_motion.ass",
        "output": "latin_motion_180s.mp4",
        "source": "testsrc2=size=960x540:rate=2:duration=180",
    },
    {
        "id": "synthetic_mixed_multiline",
        "ass": "mixed_multiline.ass",
        "output": "mixed_multiline_180s.mp4",
        "source": (
            "gradients=size=960x540:rate=2:duration=180:"
            "c0=0x18202a:c1=0x39506b:x0=0:y0=0:x1=960:y1=540"
        ),
    },
)


def _sha256(path: Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def _probe_duration(ffprobe: str, path: Path) -> float:
    result = subprocess.run(
        [
            ffprobe,
            "-v",
            "error",
            "-show_entries",
            "format=duration",
            "-of",
            "default=noprint_wrappers=1:nokey=1",
            str(path),
        ],
        capture_output=True,
        text=True,
        check=True,
    )
    return float(result.stdout.strip())


def generate_assets(
    *,
    output_dir: Path = DEFAULT_OUTPUT_DIR,
    force: bool = False,
) -> dict[str, Any]:
    ffmpeg = shutil.which("ffmpeg")
    ffprobe = shutil.which("ffprobe")
    if ffmpeg is None or ffprobe is None:
        raise RuntimeError("ffmpeg and ffprobe are required")
    output_dir.mkdir(parents=True, exist_ok=True)

    version = subprocess.run(
        [ffmpeg, "-version"],
        capture_output=True,
        text=True,
        check=True,
    ).stdout.splitlines()[0]
    generated: list[dict[str, Any]] = []
    for recipe in RECIPES:
        ass_path = FIXTURE_DIR / recipe["ass"]
        output_path = output_dir / recipe["output"]
        if not ass_path.is_file():
            raise RuntimeError(f"missing subtitle recipe: {ass_path}")
        should_generate = force or not output_path.is_file()
        if not should_generate:
            duration = _probe_duration(ffprobe, output_path)
            should_generate = abs(duration - DURATION_SECONDS) > 0.05
        if should_generate:
            command = [
                ffmpeg,
                "-hide_banner",
                "-loglevel",
                "error",
                "-y",
                "-f",
                "lavfi",
                "-i",
                recipe["source"],
                "-vf",
                f"subtitles=filename={ass_path}",
                "-an",
                "-c:v",
                "libx264",
                "-preset",
                "veryfast",
                "-crf",
                "18",
                "-pix_fmt",
                "yuv420p",
                "-threads",
                "1",
                "-map_metadata",
                "-1",
                "-movflags",
                "+faststart",
                str(output_path),
            ]
            subprocess.run(command, cwd=REPO_ROOT, check=True)

        duration = _probe_duration(ffprobe, output_path)
        if abs(duration - DURATION_SECONDS) > 0.05:
            raise RuntimeError(
                f"generated duration mismatch for {output_path}: {duration}"
            )
        generated.append(
            {
                "id": recipe["id"],
                "path": str(output_path),
                "sha256": _sha256(output_path),
                "bytes": output_path.stat().st_size,
                "duration_seconds": duration,
                "ass_sha256": _sha256(ass_path),
            }
        )
    return {
        "schema_version": 1,
        "ffmpeg": version,
        "output_dir": str(output_dir),
        "assets": generated,
    }


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Generate local Paddle multi-source quality clips"
    )
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT_DIR)
    parser.add_argument("--force", action="store_true")
    parser.add_argument("--report-out", type=Path, default=None)
    args = parser.parse_args()

    report = generate_assets(output_dir=args.output_dir, force=args.force)
    formatted = json.dumps(report, indent=2, ensure_ascii=False) + "\n"
    if args.report_out is not None:
        args.report_out.parent.mkdir(parents=True, exist_ok=True)
        args.report_out.write_text(formatted, encoding="utf-8")
    print(formatted, end="")


if __name__ == "__main__":
    main()
