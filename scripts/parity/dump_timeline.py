#!/usr/bin/env python3
"""Dump/check timeline parity golden (feat-06103)."""

from __future__ import annotations

import argparse
import json
import platform
import sys
from pathlib import Path
from typing import Any

from dump_config import git_branch, git_commit, git_dirty, repo_root

from sublift.pipeline.changepoint import EventType, StateEvent
from sublift.pipeline.timeline import TimelineBuilder


def default_golden_path(root: Path) -> Path:
    return root / "benchmark" / "parity" / "goldens" / "segments" / "timeline.v1.json"


def scenarios() -> list[dict[str, Any]]:
    return [
        {
            "name": "single_segment",
            "events": [
                {"event_type": "IN", "timestamp_ms": 0, "prev_end_ms": None},
                {"event_type": "OUT", "timestamp_ms": 1000, "prev_end_ms": None},
            ],
            "finalize_ms": None,
        },
        {
            "name": "change_splits",
            "events": [
                {"event_type": "IN", "timestamp_ms": 0, "prev_end_ms": None},
                {"event_type": "CHANGE", "timestamp_ms": 500, "prev_end_ms": 500},
                {"event_type": "OUT", "timestamp_ms": 1000, "prev_end_ms": None},
            ],
            "finalize_ms": None,
        },
        {
            "name": "open_finalized",
            "events": [
                {"event_type": "IN", "timestamp_ms": 0, "prev_end_ms": None},
            ],
            "finalize_ms": 2000,
        },
        {
            "name": "multiple_segments",
            "events": [
                {"event_type": "IN", "timestamp_ms": 0, "prev_end_ms": None},
                {"event_type": "OUT", "timestamp_ms": 1000, "prev_end_ms": None},
                {"event_type": "IN", "timestamp_ms": 2000, "prev_end_ms": None},
                {"event_type": "OUT", "timestamp_ms": 3000, "prev_end_ms": None},
            ],
            "finalize_ms": None,
        },
    ]


def run_scenario(sc: dict[str, Any]) -> list[dict[str, Any]]:
    b = TimelineBuilder()
    for e in sc["events"]:
        et = EventType[e["event_type"]]
        b.consume(
            StateEvent(
                event_type=et,
                timestamp_ms=int(e["timestamp_ms"]),
                prev_end_ms=e["prev_end_ms"],
            )
        )
    if sc["finalize_ms"] is not None:
        b.finalize_open_segment(int(sc["finalize_ms"]))
    return [{"start_ms": s.start_ms, "end_ms": s.end_ms} for s in b.build()]


def build_envelope(root: Path) -> dict[str, Any]:
    out_sc = []
    for sc in scenarios():
        out_sc.append(
            {
                "name": sc["name"],
                "events": sc["events"],
                "finalize_ms": sc["finalize_ms"],
                "segments": run_scenario(sc),
            }
        )
    return {
        "golden_schema_version": 1,
        "kind": "timeline",
        "oracle": {
            "oracle_commit": git_commit(root),
            "oracle_branch": git_branch(root),
            "python_version": platform.python_version(),
            "git_dirty": git_dirty(root),
        },
        "scenarios": out_sc,
    }


def golden_core(env: dict[str, Any]) -> Any:
    return [
        {
            "name": s["name"],
            "events": s["events"],
            "finalize_ms": s["finalize_ms"],
            "segments": s["segments"],
        }
        for s in env["scenarios"]
    ]


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser()
    p.add_argument("-o", "--output", type=Path, default=None)
    p.add_argument("--check", action="store_true")
    p.add_argument("--allow-dirty", action="store_true")
    args = p.parse_args(argv)
    root = repo_root()
    path = args.output or default_golden_path(root)
    if args.check:
        disk = json.loads(path.read_text(encoding="utf-8"))
        live = build_envelope(root)
        if golden_core(disk) != golden_core(live):
            print("timeline golden core mismatch", file=sys.stderr)
            return 1
        print(f"OK timeline golden ({len(disk['scenarios'])} scenarios)")
        return 0
    if git_dirty(root) and not args.allow_dirty:
        print("dirty worktree; use --allow-dirty", file=sys.stderr)
        return 1
    env = build_envelope(root)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(env, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"wrote {path}")
    for s in env["scenarios"]:
        print(f"  {s['name']}: {s['segments']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
