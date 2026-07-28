#!/usr/bin/env python3
"""Dump or check frozen Pipeline end-to-end parity golden (feat-06205).

Usage (repo root):
  uv run python scripts/parity/dump_pipeline.py
  uv run python scripts/parity/dump_pipeline.py --check
"""

from __future__ import annotations

import argparse
import hashlib
import json
import platform
import sys
from dataclasses import asdict
from pathlib import Path
from typing import Any

from dump_config import (
    ffmpeg_version_line,
    git_branch,
    git_commit,
    git_dirty,
    optional_version,
    repo_root,
)

from sublift.config import Config
from sublift.detector.fixed_region import FixedRegionDetector
from sublift.models import BoundingBox, Frame, OcrLine, OcrResult
from sublift.ocr.mock import MockOcrEngine
from sublift.pipeline.core import Pipeline

# ---------------------------------------------------------------------------
# Shared frame geometry (mirrors cpp/tests/pipeline_test_helpers.hpp)
# ---------------------------------------------------------------------------

W, H, BG, FG = 128, 64, 40, 240

PATTERNS: dict[str, dict[str, Any]] = {
    "empty": {"type": "blank"},
    "subA": {"type": "band", "x0": 48, "x1": 80, "y0": 44, "y1": 60},
    "subB": {"type": "band", "x0": 40, "x1": 88, "y0": 46, "y1": 58},
    # Chromatic (R≠B): feed RGB→BGR quirk required for presence/IN. Without
    # conversion fg_ratio stays 0 (calibrated vs gray subA which is BGR-noop).
    "subCyan": {
        "type": "band",
        "x0": 48,
        "x1": 80,
        "y0": 44,
        "y1": 60,
        "rgb": [0, 255, 255],
    },
}

DEFAULT_CONFIG_OVERRIDES: dict[str, Any] = {
    "change_point.enable_ssim_patrol": False,
}


def make_image(pattern: str) -> Any:
    from PIL import Image

    img = Image.new("RGB", (W, H), (BG, BG, BG))
    p = PATTERNS[pattern]
    if p["type"] == "band":
        rgb = p.get("rgb")
        color = tuple(rgb) if rgb is not None else (FG, FG, FG)
        for y in range(p["y0"], p["y1"]):
            for x in range(p["x0"], p["x1"]):
                img.putpixel((x, y), color)
    return img


def make_frame(ts: int, pattern: str) -> Frame:
    return Frame(timestamp_ms=ts, image=make_image(pattern))


# ---------------------------------------------------------------------------
# Config override (frozen dataclass -> object.__setattr__)
# ---------------------------------------------------------------------------


def apply_override(cfg: Config, path: str, value: Any) -> None:
    parts = path.split(".")
    obj: Any = cfg
    for p in parts[:-1]:
        obj = getattr(obj, p)
    object.__setattr__(obj, parts[-1], value)


def build_config(overrides: dict[str, Any]) -> Config:
    cfg = Config()
    for k, v in DEFAULT_CONFIG_OVERRIDES.items():
        apply_override(cfg, k, v)
    for k, v in overrides.items():
        apply_override(cfg, k, v)
    return cfg


def config_fingerprint(cfg: Config) -> str:
    canonical = json.dumps(asdict(cfg), sort_keys=True, separators=(",", ":"), ensure_ascii=False)
    return "sha256:" + hashlib.sha256(canonical.encode("utf-8")).hexdigest()


# ---------------------------------------------------------------------------
# Mock OCR + Detector
# ---------------------------------------------------------------------------


def build_mock(setup: dict[str, Any]) -> MockOcrEngine:
    results: list[OcrResult] = []
    for r in setup["results"]:
        if r.get("lines"):
            lines = [
                OcrLine(text=ln["text"], confidence=ln["confidence"], box=BoundingBox(**ln["box"]))
                for ln in r["lines"]
            ]
            results.append(OcrResult.from_lines(lines))
        else:
            results.append(OcrResult(text=r["text"], confidence=r["confidence"]))
    if setup["mode"] == "fixed":
        first = results[0]
        return MockOcrEngine(text=first.text, confidence=first.confidence)
    # sequence mode: pad with the last result to avoid IndexError on over-call.
    # expected_ocr_calls is computed after running, so we cannot size exactly
    # here; pad to a generous bound (len(results) + 8).
    padded = list(results)
    if padded:
        while len(padded) < len(results) + 8:
            padded.append(padded[-1])
    return MockOcrEngine(sequence=padded)


def build_detector(kind: str) -> FixedRegionDetector:
    assert kind == "fixed_full", f"unknown detector kind: {kind}"
    return FixedRegionDetector(BoundingBox(x=0, y=0, width=W, height=H))


# ---------------------------------------------------------------------------
# Scenario definitions (6 scenarios)
# ---------------------------------------------------------------------------

SCENARIOS: list[dict[str, Any]] = [
    {
        "name": "single_in_out",
        "config_overrides": {
            "enable_line_select": False,
            "confidence_threshold": 0.0,
            "min_duration_ms": 0,
        },
        "frames": [
            {"ts": 0, "pattern": "empty"},
            {"ts": 200, "pattern": "subA"},
            {"ts": 400, "pattern": "subA"},
            {"ts": 600, "pattern": "empty"},
        ],
        "detector": {"kind": "fixed_full"},
        "mock_ocr_setup": {
            "mode": "sequence",
            "results": [{"text": "你好", "confidence": 0.9, "lines": []}],
        },
    },
    {
        "name": "change_two_segments",
        "config_overrides": {
            "enable_line_select": False,
            "confidence_threshold": 0.0,
            "min_duration_ms": 0,
            "change_point.change_threshold": 5,
        },
        "frames": [
            {"ts": 0, "pattern": "empty"},
            {"ts": 200, "pattern": "subA"},
            {"ts": 400, "pattern": "subB"},
            {"ts": 600, "pattern": "subB"},
            {"ts": 800, "pattern": "empty"},
        ],
        "detector": {"kind": "fixed_full"},
        "mock_ocr_setup": {
            "mode": "sequence",
            "results": [
                {"text": "旧字", "confidence": 0.9, "lines": []},
                {"text": "新字", "confidence": 0.85, "lines": []},
            ],
        },
    },
    {
        "name": "line_select_on",
        "config_overrides": {
            "enable_line_select": True,
            "confidence_threshold": 0.5,
            "ocr_anchor_delay_frames": 0,
            "min_duration_ms": 0,
            "subtitle_script": "cjk",
        },
        "frames": [
            {"ts": 0, "pattern": "empty"},
            {"ts": 200, "pattern": "subA"},
            {"ts": 400, "pattern": "subA"},
            {"ts": 600, "pattern": "empty"},
        ],
        "detector": {"kind": "fixed_full"},
        "mock_ocr_setup": {
            "mode": "sequence",
            "results": [
                {
                    "text": "",
                    "confidence": 0.0,
                    "lines": [
                        {
                            "text": "你好世界",
                            "confidence": 0.9,
                            "box": {"x": 48, "y": 44, "width": 32, "height": 16},
                        }
                    ],
                },
            ],
        },
    },
    {
        "name": "cancel_does_not_pollute",
        "config_overrides": {
            "enable_line_select": False,
            "confidence_threshold": 0.0,
            "min_duration_ms": 0,
        },
        "frames": [
            {"ts": 0, "pattern": "empty"},
            {"ts": 200, "pattern": "subA"},
            {"ts": 400, "pattern": "subA"},
            {"ts": 600, "pattern": "empty"},
            {"ts": 800, "action": "cancel", "pattern": "empty"},
        ],
        "detector": {"kind": "fixed_full"},
        "mock_ocr_setup": {
            "mode": "sequence",
            "results": [{"text": "x", "confidence": 0.9, "lines": []}],
        },
    },
    {
        "name": "finalize_open_segment",
        "config_overrides": {
            "enable_line_select": False,
            "confidence_threshold": 0.0,
            "min_duration_ms": 0,
        },
        "frames": [
            {"ts": 0, "pattern": "empty"},
            {"ts": 200, "pattern": "subA"},
            {"ts": 400, "pattern": "subA"},
        ],
        "detector": {"kind": "fixed_full"},
        "mock_ocr_setup": {
            "mode": "sequence",
            "results": [{"text": "尾段", "confidence": 0.88, "lines": []}],
        },
    },
    {
        "name": "single_in_out_default_delay",
        "config_overrides": {
            "enable_line_select": False,
            "confidence_threshold": 0.0,
            "min_duration_ms": 0,
        },
        "frames": [
            {"ts": 0, "pattern": "empty"},
            {"ts": 200, "pattern": "subA"},
            {"ts": 400, "pattern": "subA"},
            {"ts": 600, "pattern": "subA"},
            {"ts": 800, "pattern": "subA"},
            {"ts": 1000, "pattern": "empty"},
        ],
        "detector": {"kind": "fixed_full"},
        "mock_ocr_setup": {
            "mode": "sequence",
            "results": [{"text": "延迟锚", "confidence": 0.92, "lines": []}],
        },
    },
    {
        # Missing Pipeline RGB→BGR would yield no IN (fg=0 on cyan without quirk).
        "name": "chromatic_bgr_quirk_in_out",
        "config_overrides": {
            "enable_line_select": False,
            "confidence_threshold": 0.0,
            "min_duration_ms": 0,
        },
        "frames": [
            {"ts": 0, "pattern": "empty"},
            {"ts": 200, "pattern": "subCyan"},
            {"ts": 400, "pattern": "subCyan"},
            {"ts": 600, "pattern": "empty"},
        ],
        "detector": {"kind": "fixed_full"},
        "mock_ocr_setup": {
            "mode": "sequence",
            "results": [{"text": "青带", "confidence": 0.91, "lines": []}],
        },
    },
    {
        # Two adjacent same-text segments; finalize merge_entries collapses to one.
        # expected_final_entries must diverge from raw under real merge rules.
        "name": "merge_adjacent_same_text",
        "config_overrides": {
            "enable_line_select": False,
            "confidence_threshold": 0.0,
            "min_duration_ms": 0,
            "merge_gap_ms": 1000,
        },
        "frames": [
            {"ts": 0, "pattern": "empty"},
            {"ts": 200, "pattern": "subA"},
            {"ts": 400, "pattern": "subA"},
            {"ts": 600, "pattern": "empty"},
            {"ts": 800, "pattern": "subA"},
            {"ts": 1000, "pattern": "subA"},
            {"ts": 1200, "pattern": "empty"},
        ],
        "detector": {"kind": "fixed_full"},
        "mock_ocr_setup": {
            "mode": "sequence",
            "results": [
                {"text": "同一段", "confidence": 0.9, "lines": []},
                {"text": "同一段", "confidence": 0.88, "lines": []},
            ],
        },
    },
]


# ---------------------------------------------------------------------------
# Run a scenario against the Python Pipeline oracle
# ---------------------------------------------------------------------------


def run_scenario(sc: dict[str, Any]) -> dict[str, Any]:
    cfg = build_config(sc.get("config_overrides", {}))
    mock = build_mock(sc["mock_ocr_setup"])
    det = build_detector(sc["detector"]["kind"])
    pipe = Pipeline(det, mock, cfg)

    seg_events: list[Any] = []
    raw_entries: list[dict[str, Any]] = []
    final_entries: list[dict[str, Any]] = []
    finalized = False

    for fr in sc["frames"]:
        action = fr.get("action", "feed")
        if action == "cancel":
            pipe.cancel()
            continue
        if action == "finalize":
            out = pipe.finalize()
            final_entries = [
                {
                    "start_ms": e.start_ms,
                    "end_ms": e.end_ms,
                    "text": e.text,
                    "confidence": e.confidence,
                }
                for e in out
            ]
            finalized = True
            continue
        frame = make_frame(fr["ts"], fr["pattern"])
        ev = pipe.feed(frame)
        if ev is not None:
            seg_events.append(ev)
            entry = pipe.ocr_segment(ev)
            raw_entries.append(
                {
                    "start_ms": entry.start_ms,
                    "end_ms": entry.end_ms,
                    "text": entry.text,
                    "confidence": entry.confidence,
                }
            )

    if not finalized:
        out = pipe.finalize()
        final_entries = [
            {"start_ms": e.start_ms, "end_ms": e.end_ms, "text": e.text, "confidence": e.confidence}
            for e in out
        ]

    return {
        "name": sc["name"],
        "expected_segment_events": [
            {
                "start_ms": e.start_ms,
                "end_ms": e.end_ms,
                "anchor_ts": (e.anchor_frame.timestamp_ms if e.anchor_frame is not None else None),
                "fallback_ts": [f.timestamp_ms for f in e.fallback_frames],
            }
            for e in seg_events
        ],
        "expected_raw_entries": raw_entries,
        "expected_ocr_calls": mock._index,
        "expected_final_entries": final_entries,
    }


# ---------------------------------------------------------------------------
# Envelope assembly
# ---------------------------------------------------------------------------


def default_golden_path(root: Path) -> Path:
    return root / "benchmark" / "parity" / "goldens" / "pipeline" / "pipeline.v1.json"


def build_envelope(root: Path) -> dict[str, Any]:
    scenarios_out: list[dict[str, Any]] = []
    for sc in SCENARIOS:
        result = run_scenario(sc)
        merged = {**sc, **result}
        scenarios_out.append(merged)

    cfg = build_config({})
    oracle: dict[str, Any] = {
        "oracle_commit": git_commit(root),
        "oracle_branch": git_branch(root),
        "python_version": platform.python_version(),
        "opencv_version": optional_version("cv2"),
        "numpy_version": optional_version("numpy"),
        "ffmpeg_version": ffmpeg_version_line(),
        "macos_version": platform.mac_ver()[0] if platform.system() == "Darwin" else None,
        "vision_note": None,
        "git_dirty": git_dirty(root),
        "config_fingerprint": config_fingerprint(cfg),
    }
    return {
        "golden_schema_version": 1,
        "kind": "pipeline",
        "oracle": oracle,
        "frame_geometry": {
            "width": W,
            "height": H,
            "pixel_format": "rgb24",
            "bg_value": BG,
            "fg_value": FG,
            "patterns": PATTERNS,
        },
        "default_config_overrides": DEFAULT_CONFIG_OVERRIDES,
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
        "frame_geometry": envelope["frame_geometry"],
        "default_config_overrides": envelope["default_config_overrides"],
        "scenarios": [
            {
                "name": s["name"],
                "config_overrides": s.get("config_overrides", {}),
                "frames": s["frames"],
                "detector": s["detector"],
                "mock_ocr_setup": s["mock_ocr_setup"],
                "expected_segment_events": s["expected_segment_events"],
                "expected_raw_entries": s["expected_raw_entries"],
                "expected_ocr_calls": s["expected_ocr_calls"],
                "expected_final_entries": s["expected_final_entries"],
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
    if disk.get("kind") != "pipeline":
        print("kind must be 'pipeline'", file=sys.stderr)
        return 1
    if golden_core(disk) != golden_core(live):
        print("pipeline golden core differs from live oracle", file=sys.stderr)
        print(f"  disk oracle_commit: {disk.get('oracle', {}).get('oracle_commit')}")
        print(f"  live oracle_commit: {live['oracle']['oracle_commit']}")
        disk_by = {s["name"]: s for s in disk.get("scenarios", [])}
        live_by = {s["name"]: s for s in live["scenarios"]}
        for name in sorted(set(disk_by) | set(live_by)):
            if disk_by.get(name) != live_by.get(name):
                print(f"  scenario {name}:", file=sys.stderr)
                print(
                    f"    disk: {json.dumps(disk_by.get(name, {}), ensure_ascii=False)}",
                    file=sys.stderr,
                )
                print(
                    f"    live: {json.dumps(live_by.get(name, {}), ensure_ascii=False)}",
                    file=sys.stderr,
                )
        return 1
    n = len(disk["scenarios"])
    commit = disk.get("oracle", {}).get("oracle_commit")
    print(f"OK pipeline golden matches oracle ({n} scenarios, oracle_commit={commit})")
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
            "refusing to write pipeline golden on a dirty worktree (use --allow-dirty to override)",
            file=sys.stderr,
        )
        return 1
    envelope = build_envelope(root)
    write_golden(path, envelope)
    print(f"wrote {path}")
    print(f"oracle_commit={envelope['oracle']['oracle_commit']}")
    print(f"git_dirty={envelope['oracle']['git_dirty']}")
    for s in envelope["scenarios"]:
        print(
            f"  {s['name']}: {len(s['expected_segment_events'])} events, "
            f"{s['expected_ocr_calls']} ocr_calls, "
            f"{len(s['expected_final_entries'])} final entries"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
