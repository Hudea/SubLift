#!/usr/bin/env python3
"""Dump/check dedupe parity golden (feat-06104)."""

from __future__ import annotations

import argparse
import json
import platform
import sys
from pathlib import Path
from typing import Any

from dump_config import git_branch, git_commit, git_dirty, repo_root

from sublift.models import SubtitleEntry
from sublift.pipeline.dedupe import merge_entries


def default_golden_path(root: Path) -> Path:
    return root / "benchmark" / "parity" / "goldens" / "entries" / "dedupe.v1.json"


def scenarios() -> list[dict[str, Any]]:
    return [
        {
            "name": "merge_same_adjacent",
            "merge_gap_ms": 1000,
            "min_duration_ms": 0,
            "drop_empty_text": False,
            "entries": [
                {"start_ms": 0, "end_ms": 1000, "text": "你好", "confidence": 1.0},
                {"start_ms": 1100, "end_ms": 2000, "text": "你好", "confidence": 0.9},
            ],
        },
        {
            "name": "no_merge_gap",
            "merge_gap_ms": 1000,
            "min_duration_ms": 0,
            "drop_empty_text": False,
            "entries": [
                {"start_ms": 0, "end_ms": 1000, "text": "你好", "confidence": 1.0},
                {"start_ms": 3000, "end_ms": 4000, "text": "你好", "confidence": 1.0},
            ],
        },
        {
            "name": "filter_short",
            "merge_gap_ms": 1000,
            "min_duration_ms": 500,
            "drop_empty_text": False,
            "entries": [
                {"start_ms": 0, "end_ms": 1000, "text": "长", "confidence": 1.0},
                {"start_ms": 1100, "end_ms": 1200, "text": "短", "confidence": 1.0},
            ],
        },
        {
            "name": "drop_empty_true",
            "merge_gap_ms": 1000,
            "min_duration_ms": 0,
            "drop_empty_text": True,
            "entries": [
                {"start_ms": 0, "end_ms": 1000, "text": "有字", "confidence": 1.0},
                {"start_ms": 1100, "end_ms": 2000, "text": "  ", "confidence": 1.0},
            ],
        },
        {
            "name": "drop_empty_false_keep",
            "merge_gap_ms": 1000,
            "min_duration_ms": 0,
            "drop_empty_text": False,
            "entries": [
                {"start_ms": 0, "end_ms": 1000, "text": "有字", "confidence": 1.0},
                {"start_ms": 1100, "end_ms": 2000, "text": "  ", "confidence": 1.0},
            ],
        },
        {
            "name": "whitespace_normalize_merge",
            "merge_gap_ms": 1000,
            "min_duration_ms": 0,
            "drop_empty_text": False,
            "entries": [
                {"start_ms": 0, "end_ms": 1000, "text": "你好", "confidence": 1.0},
                {"start_ms": 1000, "end_ms": 2000, "text": " 你 好 ", "confidence": 0.8},
            ],
        },
        {
            "name": "empty_between_same_then_merge",
            "merge_gap_ms": 1000,
            "min_duration_ms": 0,
            "drop_empty_text": True,
            "entries": [
                {"start_ms": 0, "end_ms": 1000, "text": "你好", "confidence": 1.0},
                {"start_ms": 1000, "end_ms": 2000, "text": "", "confidence": 1.0},
                {"start_ms": 2000, "end_ms": 3000, "text": "你好", "confidence": 1.0},
            ],
        },
        {
            "name": "jitter_short_middle_remerge",
            "merge_gap_ms": 1000,
            "min_duration_ms": 500,
            "drop_empty_text": False,
            "entries": [
                {"start_ms": 0, "end_ms": 1000, "text": "你好", "confidence": 1.0},
                {"start_ms": 1000, "end_ms": 1100, "text": "你妳", "confidence": 1.0},
                {"start_ms": 1100, "end_ms": 2000, "text": "你好", "confidence": 1.0},
            ],
        },
        {
            "name": "two_short_same_merge_then_keep",
            "merge_gap_ms": 1000,
            "min_duration_ms": 500,
            "drop_empty_text": False,
            "entries": [
                {"start_ms": 0, "end_ms": 300, "text": "你好", "confidence": 1.0},
                {"start_ms": 400, "end_ms": 700, "text": "你好", "confidence": 1.0},
            ],
        },
        {
            "name": "three_pass_chain",
            "merge_gap_ms": 1000,
            "min_duration_ms": 500,
            "drop_empty_text": False,
            "entries": [
                {"start_ms": 0, "end_ms": 300, "text": "你好", "confidence": 1.0},
                {"start_ms": 400, "end_ms": 700, "text": "你好", "confidence": 1.0},
                {"start_ms": 800, "end_ms": 1100, "text": "你好", "confidence": 1.0},
            ],
        },
    ]


def run(sc: dict[str, Any]) -> list[dict[str, Any]]:
    entries = [
        SubtitleEntry(
            start_ms=e["start_ms"],
            end_ms=e["end_ms"],
            text=e["text"],
            confidence=e["confidence"],
        )
        for e in sc["entries"]
    ]
    out = merge_entries(
        entries,
        merge_gap_ms=sc["merge_gap_ms"],
        min_duration_ms=sc["min_duration_ms"],
        drop_empty_text=sc["drop_empty_text"],
    )
    return [
        {
            "start_ms": e.start_ms,
            "end_ms": e.end_ms,
            "text": e.text,
            "confidence": e.confidence,
        }
        for e in out
    ]


def build_envelope(root: Path) -> dict[str, Any]:
    scs = []
    for sc in scenarios():
        scs.append({**sc, "result": run(sc)})
    return {
        "golden_schema_version": 1,
        "kind": "dedupe",
        "oracle": {
            "oracle_commit": git_commit(root),
            "oracle_branch": git_branch(root),
            "python_version": platform.python_version(),
            "git_dirty": git_dirty(root),
        },
        "scenarios": scs,
    }


def golden_core(env: dict[str, Any]) -> Any:
    return env["scenarios"]


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
            print("dedupe golden core mismatch", file=sys.stderr)
            return 1
        print(f"OK dedupe golden ({len(disk['scenarios'])} scenarios)")
        return 0
    if git_dirty(root) and not args.allow_dirty:
        print("dirty worktree; use --allow-dirty", file=sys.stderr)
        return 1
    env = build_envelope(root)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(env, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"wrote {path}")
    for s in env["scenarios"]:
        print(f"  {s['name']}: {s['result']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
