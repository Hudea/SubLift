"""Media helpers for offline tools (ffprobe only)."""

from __future__ import annotations

import json
import subprocess
from pathlib import Path


def probe_duration_seconds(video_path: Path) -> float:
    """Probe duration with ffprobe. Raises RuntimeError if probing fails."""
    try:
        result = subprocess.run(
            [
                "ffprobe",
                "-v",
                "error",
                "-show_entries",
                "format=duration",
                "-of",
                "json",
                str(video_path),
            ],
            capture_output=True,
            text=True,
            check=True,
        )
        payload = json.loads(result.stdout)
        duration = float(payload["format"]["duration"])
        if duration <= 0:
            raise RuntimeError(f"invalid duration from ffprobe: {duration}")
        return duration
    except (OSError, subprocess.CalledProcessError, KeyError, TypeError, ValueError) as exc:
        raise RuntimeError(f"ffprobe 无法读取时长: {video_path}") from exc
