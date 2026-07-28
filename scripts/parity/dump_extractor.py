#!/usr/bin/env python3
"""Dump/check extractor parity golden (feat-06305)."""

from __future__ import annotations

import argparse
import json
import platform
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any
from unittest.mock import patch

from dump_config import git_branch, git_commit, git_dirty, repo_root

from sublift.extractor.ffmpeg_extractor import (
    FfmpegExtractor,
    SourceFrameInfo,
    _assess_display_transform,
    _ffmpeg_bin,
    _ffprobe_bin,
    validate_output_crop,
)
from sublift.extractor.frame_io import build_output_vf, plan_frame_io
from sublift.models import BoundingBox, Frame


def default_golden_path(root: Path) -> Path:
    return root / "benchmark" / "parity" / "goldens" / "extractor" / "extractor.v1.json"


def sample_frame_pixels(frame: Frame) -> list[dict[str, Any]]:
    img = frame.image
    w, h = img.width, img.height
    if w <= 0 or h <= 0:
        return []
    coords = [
        (0, 0),
        (w - 1, 0),
        (0, h - 1),
        (w - 1, h - 1),
        (w // 2, h // 2),
    ]
    sampled = []
    for x, y in coords:
        px = img.getpixel((x, y))
        if isinstance(px, int):
            r = g = b = px
        else:
            r, g, b = int(px[0]), int(px[1]), int(px[2])
        sampled.append({"x": x, "y": y, "rgb": [r, g, b]})
    return sampled


def detector_type_key(detector: object) -> str:
    return detector.__class__.__name__.lower().replace("detector", "")


def plan_entry(
    name: str,
    region_box: BoundingBox | None,
    mode: str,
    *,
    source: SourceFrameInfo | None = None,
    on_unvalidated_transform: str = "fallback_full",
    expected_ok: bool = True,
    expected_error_contains: str | None = None,
) -> dict[str, Any]:
    """Build one pure_plan_frame_io row (optionally with mocked probe source)."""
    source_json: dict[str, Any] | None = None
    if source is not None:
        source_json = {
            "width": source.width,
            "height": source.height,
            "display_transform_ok": source.display_transform_ok,
            "transform_note": source.transform_note,
        }

    base: dict[str, Any] = {
        "name": name,
        "region_box": (
            {
                "x": region_box.x,
                "y": region_box.y,
                "width": region_box.width,
                "height": region_box.height,
            }
            if region_box is not None
            else None
        ),
        "mode": mode,
        "source": source_json,
        "on_unvalidated_transform": on_unvalidated_transform,
        "expected_ok": expected_ok,
        "expected_error_contains": expected_error_contains,
    }

    if not expected_ok:
        # Still exercise oracle to confirm error class
        try:
            if source is not None:
                with patch(
                    "sublift.extractor.frame_io.probe_source_frame",
                    return_value=source,
                ):
                    plan_frame_io(
                        Path("/tmp/fake.mp4"),
                        region_box,
                        mode=mode,  # type: ignore[arg-type]
                        on_unvalidated_transform=on_unvalidated_transform,  # type: ignore[arg-type]
                    )
            else:
                plan_frame_io(
                    Path("/tmp/fake.mp4"),
                    region_box,
                    mode=mode,  # type: ignore[arg-type]
                    on_unvalidated_transform=on_unvalidated_transform,  # type: ignore[arg-type]
                )
            raise RuntimeError(f"expected plan_frame_io to fail for {name}")
        except Exception as exc:
            msg = str(exc)
            if expected_error_contains and expected_error_contains not in msg:
                raise AssertionError(
                    f"{name}: error {msg!r} missing {expected_error_contains!r}"
                ) from exc
        base["expected_output_crop"] = None
        base["expected_output_mode"] = None
        base["expected_detector_type"] = None
        base["expected_fallback_reason"] = None
        return base

    if source is not None:
        with patch(
            "sublift.extractor.frame_io.probe_source_frame",
            return_value=source,
        ):
            plan = plan_frame_io(
                Path("/tmp/fake.mp4"),
                region_box,
                mode=mode,  # type: ignore[arg-type]
                on_unvalidated_transform=on_unvalidated_transform,  # type: ignore[arg-type]
            )
    else:
        plan = plan_frame_io(
            Path("/tmp/fake.mp4"),
            region_box,
            mode=mode,  # type: ignore[arg-type]
            on_unvalidated_transform=on_unvalidated_transform,  # type: ignore[arg-type]
        )

    base["expected_output_crop"] = (
        {
            "x": plan.output_crop.x,
            "y": plan.output_crop.y,
            "width": plan.output_crop.width,
            "height": plan.output_crop.height,
        }
        if plan.output_crop
        else None
    )
    base["expected_output_mode"] = plan.output_mode
    base["expected_detector_type"] = detector_type_key(plan.detector)
    base["expected_fallback_reason"] = plan.fallback_reason
    return base


def build_envelope(root: Path) -> dict[str, Any]:
    ffmpeg_path = _ffmpeg_bin()
    ffprobe_path = _ffprobe_bin()

    try:
        ffmpeg_ver = subprocess.check_output([ffmpeg_path, "-version"], text=True).splitlines()[0]
    except Exception:
        ffmpeg_ver = "unknown"

    try:
        ffprobe_ver = subprocess.check_output([ffprobe_path, "-version"], text=True).splitlines()[0]
    except Exception:
        ffprobe_ver = "unknown"

    pure_validate_crop = []
    for name, crop, sw, sh, ok, err_substr in [
        ("valid_crop", BoundingBox(10, 10, 160, 120), 320, 240, True, None),
        (
            "edge_touch",
            BoundingBox(0, 0, 320, 240),
            320,
            240,
            True,
            None,
        ),
        (
            "negative_x",
            BoundingBox(-1, 10, 160, 120),
            320,
            240,
            False,
            "output_crop 的 x/y 不能为负",
        ),
        (
            "negative_y",
            BoundingBox(10, -1, 160, 120),
            320,
            240,
            False,
            "output_crop 的 x/y 不能为负",
        ),
        (
            "zero_width",
            BoundingBox(0, 0, 0, 120),
            320,
            240,
            False,
            "output_crop 的 width/height 必须为正",
        ),
        (
            "zero_height",
            BoundingBox(0, 0, 160, 0),
            320,
            240,
            False,
            "output_crop 的 width/height 必须为正",
        ),
        (
            "out_of_bounds",
            BoundingBox(200, 100, 160, 180),
            320,
            240,
            False,
            "output_crop 越界",
        ),
        (
            "overflow_y",
            BoundingBox(0, 200, 160, 50),
            320,
            240,
            False,
            "output_crop 越界",
        ),
    ]:
        item: dict[str, Any] = {
            "name": name,
            "crop": (
                {
                    "x": crop.x,
                    "y": crop.y,
                    "width": crop.width,
                    "height": crop.height,
                }
                if crop
                else None
            ),
            "source_width": sw,
            "source_height": sh,
            "expected_ok": ok,
            "expected_error_contains": err_substr,
        }
        pure_validate_crop.append(item)

    pure_build_vf = []
    for name, fps, crop in [
        ("full_frame_1fps", 1.0, None),
        (
            "roi_crop_1fps",
            1.0,
            BoundingBox(10, 10, 160, 120),
        ),
    ]:
        pure_build_vf.append(
            {
                "name": name,
                "fps": fps,
                "crop": (
                    {
                        "x": crop.x,
                        "y": crop.y,
                        "width": crop.width,
                        "height": crop.height,
                    }
                    if crop
                    else None
                ),
                "expected_vf": build_output_vf(fps, crop),
            }
        )

    pure_assess_display = []
    for name, stream_json, _exp_ok, _exp_note in [
        (
            "identity_stream",
            {"width": 320, "height": 240, "tags": {"rotate": "0"}},
            True,
            None,
        ),
        (
            "rotate_90_tag",
            {"width": 320, "height": 240, "tags": {"rotate": "90"}},
            False,
            "stream.tags.rotate='90'",
        ),
        (
            "fixed_point_identity_side_data",
            {
                "width": 320,
                "height": 240,
                "side_data_list": [
                    {
                        "side_data_type": "Display Matrix",
                        "displaymatrix": "65536 0 0 0 65536 0 0 0 1073741824",
                    }
                ],
            },
            True,
            None,
        ),
    ]:
        ok, note = _assess_display_transform(stream_json)
        pure_assess_display.append(
            {
                "name": name,
                "stream_json": stream_json,
                "expected_ok": ok,
                "expected_note": note,
            }
        )

    region_roi = BoundingBox(0, 180, 320, 60)
    region_full = BoundingBox(10, 10, 160, 120)
    ok_source = SourceFrameInfo(
        width=320, height=240, display_transform_ok=True, transform_note=None
    )
    bad_source = SourceFrameInfo(
        width=320,
        height=240,
        display_transform_ok=False,
        transform_note="rotate=90",
    )

    pure_plan_frame_io = [
        plan_entry("no_region_auto", None, "auto"),
        plan_entry("region_full", region_full, "full"),
        plan_entry(
            "region_auto_roi_success",
            region_roi,
            "auto",
            source=ok_source,
        ),
        plan_entry(
            "region_roi_success",
            region_roi,
            "roi",
            source=ok_source,
            on_unvalidated_transform="error",
        ),
        plan_entry(
            "auto_transform_fallback",
            region_roi,
            "auto",
            source=bad_source,
            on_unvalidated_transform="fallback_full",
        ),
        plan_entry(
            "roi_transform_error",
            region_roi,
            "roi",
            source=bad_source,
            on_unvalidated_transform="error",
            expected_ok=False,
            expected_error_contains="显示变换",
        ),
        plan_entry(
            "auto_transform_error_policy",
            region_roi,
            "auto",
            source=bad_source,
            on_unvalidated_transform="error",
            expected_ok=False,
            expected_error_contains="显示变换",
        ),
    ]

    # Synthetic video extraction scenarios
    extract_scenarios = []
    with tempfile.NamedTemporaryFile(suffix=".mp4") as tmp_file:
        tmp_path = Path(tmp_file.name)
        gen_cmd = [
            ffmpeg_path,
            "-nostdin",
            "-v",
            "error",
            "-f",
            "lavfi",
            "-i",
            "testsrc=size=320x240:rate=1",
            "-t",
            "3",
            "-pix_fmt",
            "yuv420p",
            "-y",
            str(tmp_path),
        ]
        subprocess.check_call(gen_cmd)

        # 1. Full 1fps
        extractor = FfmpegExtractor(fps=1.0)
        frames_1fps = list(extractor.extract(tmp_path))
        extract_scenarios.append(
            {
                "name": "extract_full_1fps",
                "fps": 1.0,
                "crop": None,
                "expected_ok": True,
                "expected_frame_count": len(frames_1fps),
                "expected_timestamps_ms": [f.timestamp_ms for f in frames_1fps],
                "expected_width": frames_1fps[0].image.width,
                "expected_height": frames_1fps[0].image.height,
                "frames": [
                    {
                        "frame_index": i,
                        "timestamp_ms": f.timestamp_ms,
                        "sampled_pixels": sample_frame_pixels(f),
                    }
                    for i, f in enumerate(frames_1fps)
                ],
            }
        )

        # 2. Full 2fps
        extractor = FfmpegExtractor(fps=2.0)
        frames_2fps = list(extractor.extract(tmp_path))
        extract_scenarios.append(
            {
                "name": "extract_full_2fps",
                "fps": 2.0,
                "crop": None,
                "expected_ok": True,
                "expected_frame_count": len(frames_2fps),
                "expected_timestamps_ms": [f.timestamp_ms for f in frames_2fps],
                "expected_width": frames_2fps[0].image.width,
                "expected_height": frames_2fps[0].image.height,
                "frames": [],
            }
        )

        # 3. ROI Crop (with corner/center pixel L0 samples)
        crop_box = BoundingBox(10, 10, 160, 120)
        extractor = FfmpegExtractor(fps=1.0, output_crop=crop_box)
        frames_roi = list(extractor.extract(tmp_path))
        extract_scenarios.append(
            {
                "name": "extract_roi_crop",
                "fps": 1.0,
                "crop": {
                    "x": crop_box.x,
                    "y": crop_box.y,
                    "width": crop_box.width,
                    "height": crop_box.height,
                },
                "expected_ok": True,
                "expected_frame_count": len(frames_roi),
                "expected_timestamps_ms": [f.timestamp_ms for f in frames_roi],
                "expected_width": frames_roi[0].image.width,
                "expected_height": frames_roi[0].image.height,
                "frames": [
                    {
                        "frame_index": 0,
                        "timestamp_ms": frames_roi[0].timestamp_ms,
                        "sampled_pixels": sample_frame_pixels(frames_roi[0]),
                    }
                ],
            }
        )

        # 4. Pre-cancel
        extractor = FfmpegExtractor(fps=1.0)
        extractor.cancel()
        frames_pre_cancel = list(extractor.extract(tmp_path))
        extract_scenarios.append(
            {
                "name": "extract_cancel_pre",
                "fps": 1.0,
                "crop": None,
                "cancel_before_extract": True,
                "expected_ok": True,
                "expected_frame_count": len(frames_pre_cancel),
                "expected_timestamps_ms": [],
                "expected_width": 0,
                "expected_height": 0,
                "frames": [],
            }
        )

        # 5. Illegal crop (validate_output_crop fails at extract)
        illegal = BoundingBox(0, 0, 1000, 1000)
        # Confirm oracle error class without spawning ffmpeg decode
        try:
            validate_output_crop(illegal, 320, 240)
            raise RuntimeError("expected illegal crop to fail validation")
        except ValueError as exc:
            err_substr = "output_crop 越界"
            if err_substr not in str(exc):
                raise
        extractor = FfmpegExtractor(fps=1.0, output_crop=illegal)
        try:
            list(extractor.extract(tmp_path))
            raise RuntimeError("expected extract illegal crop to raise")
        except ValueError as exc:
            if "output_crop 越界" not in str(exc):
                raise
        extract_scenarios.append(
            {
                "name": "extract_illegal_crop",
                "fps": 1.0,
                "crop": {
                    "x": illegal.x,
                    "y": illegal.y,
                    "width": illegal.width,
                    "height": illegal.height,
                },
                "expected_ok": False,
                "expected_error_contains": "output_crop 越界",
                "expected_frame_count": 0,
                "expected_timestamps_ms": [],
                "expected_width": 0,
                "expected_height": 0,
                "frames": [],
            }
        )
        # Note: mid-flight cancel is unit-tested in extractor_full_test.cpp only
        # (timing-sensitive; not locked in frozen golden).

    return {
        "golden_schema_version": 1,
        "kind": "extractor",
        "oracle": {
            "oracle_commit": git_commit(root),
            "oracle_branch": git_branch(root),
            "python_version": platform.python_version(),
            "ffmpeg_version": ffmpeg_ver,
            "ffprobe_version": ffprobe_ver,
            "git_dirty": git_dirty(root),
        },
        "pure_validate_crop": pure_validate_crop,
        "pure_build_vf": pure_build_vf,
        "pure_assess_display": pure_assess_display,
        "pure_plan_frame_io": pure_plan_frame_io,
        "extract_scenarios": extract_scenarios,
    }


def remove_volatile_oracle_keys(d: Any) -> Any:
    if isinstance(d, dict):
        res = {}
        for k, v in d.items():
            if k == "oracle":
                continue
            res[k] = remove_volatile_oracle_keys(v)
        return res
    elif isinstance(d, list):
        return [remove_volatile_oracle_keys(item) for item in d]
    return d


def main() -> int:
    parser = argparse.ArgumentParser(description="Dump/check extractor parity golden.")
    parser.add_argument("--check", action="store_true", help="Check committed golden file")
    parser.add_argument("--output", type=Path, default=None, help="Output golden JSON file path")
    args = parser.parse_args()

    root = repo_root()
    golden_path = args.output if args.output is not None else default_golden_path(root)

    env = build_envelope(root)

    if args.check:
        if not golden_path.exists():
            print(f"FAIL: Golden file {golden_path} does not exist", file=sys.stderr)
            return 1
        with golden_path.open("r", encoding="utf-8") as f:
            disk_env = json.load(f)
        disk_clean = remove_volatile_oracle_keys(disk_env)
        live_clean = remove_volatile_oracle_keys(env)

        if disk_clean != live_clean:
            print("FAIL: Extractor golden does not match live Python oracle!", file=sys.stderr)
            return 1
        print(f"OK extractor golden matches oracle ({golden_path})")
        return 0

    golden_path.parent.mkdir(parents=True, exist_ok=True)
    with golden_path.open("w", encoding="utf-8") as f:
        json.dump(env, f, indent=2, ensure_ascii=False)
        f.write("\n")
    print(f"Successfully wrote Extractor golden to {golden_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
