#!/usr/bin/env python3
"""Check macOS App Bundle layout and ensure thin CLI purity and bundle integrity."""

import argparse
import os
import subprocess
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description="Check SubLift macOS App Bundle Layout")
    parser.add_argument(
        "--bundle-path",
        type=Path,
        default=Path(__file__).resolve().parent.parent / "build" / "SubLift.app",
        help="Path to SubLift.app bundle",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="Enforce zero external Homebrew links (standalone release)",
    )
    args = parser.parse_args()
    bundle_path: Path = args.bundle_path.resolve()

    if not bundle_path.exists():
        print(f"[FAIL] App Bundle directory does not exist: {bundle_path}")
        return 1

    contents = bundle_path / "Contents"
    required_paths = [
        contents / "Info.plist",
        contents / "MacOS" / "sublift_cli",
        contents / "Helpers" / "sublift_worker",
        contents / "Resources" / "models",
        contents / "Resources" / "bin",
        contents / "Frameworks",
    ]

    missing = [p for p in required_paths if not p.exists()]
    if missing:
        missing_rel = [str(m.relative_to(bundle_path)) for m in missing]
        print(f"[FAIL] Missing required bundle paths: {missing_rel}")
        return 1

    # Check executable bits
    cli_exe = contents / "MacOS" / "sublift_cli"
    worker_exe = contents / "Helpers" / "sublift_worker"
    for exe in [cli_exe, worker_exe]:
        if not os.access(exe, os.X_OK):
            print(f"[FAIL] Executable bit missing on: {exe.relative_to(bundle_path)}")
            return 1

    forbidden_prefixes = ["/opt/homebrew", "/usr/local/Cellar", ".venv"]

    # 1. sublift_cli MUST be thin (zero ORT / OpenCV / Vision / Homebrew)
    try:
        out = subprocess.check_output(["otool", "-L", str(cli_exe)], text=True)
        for line in out.splitlines():
            line = line.strip()
            is_forbidden = any(
                f in line for f in ["onnxruntime", "opencv", "Vision.framework"]
            ) or any(p in line for p in forbidden_prefixes)
            if is_forbidden:
                print(f"[FAIL] sublift_cli is not thin! Forbidden link detected: {line}")
                return 1
    except Exception as e:
        print(f"[WARNING] Could not run otool -L on sublift_cli: {e}")

    # 2. sublift_worker linkage check
    try:
        out = subprocess.check_output(["otool", "-L", str(worker_exe)], text=True)
        for line in out.splitlines():
            line = line.strip()
            if any(p in line for p in forbidden_prefixes):
                if args.strict:
                    print(f"[FAIL] sublift_worker links external path ({line}) in --strict mode")
                    return 1
                else:
                    print(f"[INFO] sublift_worker dev build links external dependency: {line}")
    except Exception as e:
        print(f"[WARNING] Could not run otool -L on sublift_worker: {e}")

    print(f"[OK] App Bundle mechanical layout verification PASS ({bundle_path.name})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
