#!/usr/bin/env python3
"""Dump/check line_select parity golden (feat-06105)."""

from __future__ import annotations

import argparse
import json
import platform
import sys
from pathlib import Path
from typing import Any

from dump_config import git_branch, git_commit, git_dirty, repo_root

from sublift.models import BoundingBox, OcrLine, SubtitleProfile
from sublift.pipeline.line_select import (
    cleanup_subtitle_text,
    consensus_text,
    edit_distance,
    normalize_ocr_text,
    select_line,
    should_accept_text,
)


def default_golden_path(root: Path) -> Path:
    return (
        root
        / "benchmark"
        / "parity"
        / "goldens"
        / "line_select"
        / "line_select.v1.json"
    )


def profile(**kw: Any) -> SubtitleProfile:
    return SubtitleProfile(
        script=kw.get("script", "cjk"),
        center_x=kw.get("center_x", 160),
        center_y=kw.get("center_y", 40),
        height=kw.get("height", 30),
        y_min=kw.get("y_min", 20),
        y_max=kw.get("y_max", 60),
    )


def build_envelope(root: Path) -> dict[str, Any]:
    cases: list[dict[str, Any]] = []

    # normalize / cleanup
    cases.append(
        {
            "name": "normalize_ws",
            "kind": "normalize",
            "input": "  你  好  ",
            "output": normalize_ocr_text("  你  好  "),
        }
    )
    for name, text in [
        ("cleanup_ellipsis", "你好..."),
        ("cleanup_quotes", "他们说：『没问题』"),
        ("cleanup_fact_dot", "事实上."),
        ("cleanup_strip_both_edges", "FOR一起揭穿阴谋SON"),
        ("cleanup_strip_trailing_phison", "（前市长杨咩咩入狱）PHISON"),
        ("cleanup_strip_trailing_son", "在动物方城市气候墙SON"),
        ("cleanup_strip_trailing_efs", "哈茱蒂，本市第一位兔警员EFS CONSPI _N/、"),
        ("cleanup_keep_mixed_zpd", "后来加入ZPD警局"),
        ("cleanup_keep_spaced_english", "欢迎来到 ZPD"),
        ("cleanup_keep_pure_english", "HELLO"),
    ]:
        cases.append(
            {
                "name": name,
                "kind": "cleanup",
                "input": text,
                "script": "cjk",
                "output": cleanup_subtitle_text(text, "cjk"),
            }
        )
    cases.append(
        {
            "name": "normalize_ideo_space",
            "kind": "normalize",
            "input": "你\u3000好",
            "output": normalize_ocr_text("你\u3000好"),
        }
    )

    # select_line
    prof = profile()

    def ol(text: str, conf: float, x: int, y: int, w: int, h: int) -> OcrLine:
        return OcrLine(
            text=text,
            confidence=conf,
            box=BoundingBox(x=x, y=y, width=w, height=h),
        )

    lines2 = [
        ol("BREAKING", 0.99, 10, 5, 100, 20),
        ol("你好世界", 0.9, 100, 30, 120, 28),
    ]
    sel = select_line(lines2, prof)
    cases.append(
        {
            "name": "select_prefers_cjk_in_band",
            "kind": "select_line",
            "profile": {
                "script": prof.script,
                "center_x": prof.center_x,
                "center_y": prof.center_y,
                "height": prof.height,
                "y_min": prof.y_min,
                "y_max": prof.y_max,
            },
            "lines": [
                {
                    "text": ln.text,
                    "confidence": ln.confidence,
                    "box": {
                        "x": ln.box.x,
                        "y": ln.box.y,
                        "width": ln.box.width,
                        "height": ln.box.height,
                    },
                }
                for ln in lines2
            ],
            "selected_text": None if sel is None else sel.text,
        }
    )

    cons = consensus_text(
        [("你好", 0.9), ("你好", 0.8), ("你号", 0.7)],
        script="cjk",
    )
    cases.append(
        {
            "name": "consensus_cjk",
            "kind": "consensus",
            "samples": [["你好", 0.9], ["你好", 0.8], ["你号", 0.7]],
            "script": "cjk",
            "result": {
                "text": cons.text,
                "confidence": cons.confidence,
                "support_votes": cons.support_votes,
            },
        }
    )

    cases.append(
        {
            "name": "edit_distance_kitten",
            "kind": "edit_distance",
            "a": "kitten",
            "b": "sitting",
            "distance": edit_distance("kitten", "sitting"),
        }
    )

    acc = should_accept_text(
        "你好世界",
        0.6,
        profile=prof,
        confidence_threshold=0.5,
        low_conf_threshold=0.28,
        support_votes=2,
    )
    cases.append(
        {
            "name": "should_accept_cjk",
            "kind": "should_accept",
            "text": "你好世界",
            "confidence": 0.6,
            "accept": acc,
        }
    )

    return {
        "golden_schema_version": 1,
        "kind": "line_select",
        "oracle": {
            "oracle_commit": git_commit(root),
            "oracle_branch": git_branch(root),
            "python_version": platform.python_version(),
            "git_dirty": git_dirty(root),
        },
        "cases": cases,
    }


def golden_core(env: dict[str, Any]) -> Any:
    return env["cases"]


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
            print("line_select golden core mismatch", file=sys.stderr)
            return 1
        print(f"OK line_select golden ({len(disk['cases'])} cases)")
        return 0
    if git_dirty(root) and not args.allow_dirty:
        print("dirty worktree; use --allow-dirty", file=sys.stderr)
        return 1
    env = build_envelope(root)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(env, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"wrote {path}")
    for c in env["cases"]:
        print(f"  {c['name']}: {c}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
