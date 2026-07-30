#!/usr/bin/env python3
"""Verify native sublift_cli standalone execution in a Python-free environment (env -i)."""

import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def get_or_create_test_video(root_dir: Path) -> Path:
    candidates = [
        root_dir / "tests" / "fixtures" / "zootopia_1080p_5fps_120s.mkv",
        root_dir / "tests" / "fixtures" / "sample.mp4",
        Path("/tmp/sublift_test_clip.mp4"),
    ]
    for c in candidates:
        if c.exists() and c.stat().st_size > 0:
            return c

    out_path = Path(tempfile.gettempdir()) / "sublift_synth_clip.mp4"
    if not out_path.exists():
        subprocess.run(
            [
                "ffmpeg",
                "-f",
                "lavfi",
                "-i",
                "testsrc=duration=1:size=320x240:rate=5",
                "-y",
                str(out_path),
            ],
            capture_output=True,
            check=False,
        )
    return out_path


def check_child_processes_zero_python(pid: int) -> bool:
    """Assert no python or uv processes are spawned in the child process tree."""
    try:
        # Check all child processes of the PID
        out = subprocess.check_output(
            ["ps", "-o", "command=", "-g", str(pid)], text=True, stderr=subprocess.DEVNULL
        )
        for line in out.splitlines():
            line_lower = line.lower()
            if "python" in line_lower or "uv " in line_lower:
                print(f"[FAIL] Forbidden Python/uv process detected in tree: {line}")
                return False
    except Exception:
        # Process may have already exited
        pass
    return True


def main() -> int:
    root_dir = Path(__file__).resolve().parent.parent
    cli_candidates = [
        root_dir / "build" / "SubLift.app" / "Contents" / "MacOS" / "sublift_cli",
        root_dir / "build" / "cpp" / "bin" / "sublift_cli",
    ]

    cli_path = None
    for cand in cli_candidates:
        if cand.exists() and os.access(cand, os.X_OK):
            cli_path = cand
            break

    if cli_path is None:
        print(f"[FAIL] No executable sublift_cli found in candidates: {cli_candidates}")
        return 1

    video_fixture = get_or_create_test_video(root_dir)
    if not video_fixture.exists():
        print(f"[FAIL] Test video fixture could not be resolved or created: {video_fixture}")
        return 1

    with tempfile.NamedTemporaryFile(suffix=".srt", delete=False) as tmp_srt:
        output_srt = Path(tmp_srt.name)

    # Clean environment without Python vars
    clean_env = {
        "PATH": "/usr/bin:/bin:/usr/sbin:/sbin",
        "HOME": os.getenv("HOME", "/tmp"),
        "TMPDIR": tempfile.gettempdir(),
    }

    cmd = [
        str(cli_path),
        "extract",
        str(video_fixture),
        "--engine",
        "mock",
        "-o",
        str(output_srt),
    ]

    print(f"[INFO] Testing Python-free execution: {' '.join(cmd)}")
    try:
        proc = subprocess.Popen(
            cmd,
            env=clean_env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            cwd=str(root_dir),
        )

        python_detected = False
        # Poll process and monitor process tree
        while proc.poll() is None:
            if not check_child_processes_zero_python(proc.pid):
                python_detected = True
                proc.kill()
                break
            time.sleep(0.05)

        stdout, stderr = proc.communicate(timeout=10)
        if python_detected:
            return 1

        if proc.returncode != 0:
            print(f"[FAIL] sublift_cli returned exit code {proc.returncode}")
            print(f"Stdout:\n{stdout}")
            print(f"Stderr:\n{stderr}")
            return 1

        if not output_srt.exists():
            print(f"[FAIL] Output SRT path was not created: {output_srt}")
            return 1

        print(f"[OK] Python-free standalone execution PASS ({cli_path.relative_to(root_dir)})")
        return 0
    finally:
        output_srt.unlink(missing_ok=True)


if __name__ == "__main__":
    sys.exit(main())
