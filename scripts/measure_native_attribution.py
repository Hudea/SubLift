"""Measure the C++ Native stage attribution baseline (Phase 16 D01 / S0).

Runs the Native product CLI on a fixed load with ``SUBLIFT_STAGE_TIMING=1``
(warmup + measured independent processes), parses the ``SUBLIFT_STAGE_TIMING``
JSON line from stderr, and aggregates per-stage medians. Also verifies that
the exported SRT hash is stable across runs and identical to a run with stage
timing disabled (the instrumentation must be inert).

Protocol (mirrors ``benchmark/baselines/performance-attribution-baseline.md``):
same machine, same load, same version, warmup >= 1 + measured >= 3 independent
processes, median as the primary statistic.

Usage::

    uv run python scripts/measure_native_attribution.py \
        --video debug/Zootopia_clip_1080p.mp4 --engine paddle --fps 5 \
        --script cjk --label native_paddle_bottomcrop_5fps
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import statistics
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[1]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from sublift.worker_bin import resolve_native_cli, resolve_worker_bin  # noqa: E402

STAGE_TIMING_PREFIX = "SUBLIFT_STAGE_TIMING "

# Top-level leaves of the stage tree; everything else nests under one of these
# (see emit_stage_timing in cpp/src/application/bridge.cpp).
TOP_LEVEL_LEAVES = ("extract_wait", "frame_fetch", "consume_call", "finalize")

# Stage tree for the report: stage -> parent ("" marks a top-level leaf).
STAGE_TREE: dict[str, str] = {
    "extract_wait": "",
    "frame_fetch": "",
    "frame_materialize": "frame_fetch",
    "consume_call": "",
    "consume": "consume_call",
    "signature": "consume",
    "changepoint": "consume",
    "color_convert": "consume",
    "pipeline_overhead": "consume",
    "ocr": "consume",
    "paddle.global_preprocess": "ocr",
    "paddle.crop": "ocr",
    "paddle.det_preprocess": "ocr",
    "paddle.det_infer": "ocr",
    "paddle.det_postprocess": "ocr",
    "paddle.cls_preprocess": "ocr",
    "paddle.cls_infer": "ocr",
    # Phase 20 operator-level stages.
    "paddle.cls_apply": "ocr",
    "paddle.rec_preprocess": "ocr",
    "paddle.rec_infer": "ocr",
    "paddle.rec_decode": "ocr",
    "paddle.ocr_post": "ocr",
    "finalize": "",
}


@dataclass
class StageAggregate:
    calls: int = 0
    total_ms: float = 0.0


@dataclass
class RunRecord:
    index: int
    wall_seconds: float
    core_wall_ms: float
    job_wall_ms: float
    coverage_pct: float
    srt_sha256: str
    srt_entries: int
    stages: dict[str, StageAggregate] = field(default_factory=dict)


def _parse_stage_line(stderr: str) -> dict[str, Any]:
    for line in stderr.splitlines():
        if line.startswith(STAGE_TIMING_PREFIX):
            payload: dict[str, Any] = json.loads(line[len(STAGE_TIMING_PREFIX) :])
            return payload
    raise RuntimeError("SUBLIFT_STAGE_TIMING line not found in stderr")


def _run_once(
    cli: Path,
    worker: Path | None,
    video: Path,
    *,
    engine: str,
    fps: float,
    script: str,
    confidence: float,
    stage_timing: bool,
    tag: str,
) -> tuple[RunRecord, str]:
    with tempfile.TemporaryDirectory(prefix="sublift-attrib-") as tmp:
        output = Path(tmp) / f"{tag}.srt"
        command = [
            str(cli),
            "extract",
            str(video),
            "-o",
            str(output),
            "--engine",
            engine,
            "--fps",
            str(fps),
            "--confidence",
            str(confidence),
            "--script",
            script,
        ]
        env = os.environ.copy()
        if worker is not None:
            env["SUBLIFT_WORKER_PATH"] = str(worker)
        if stage_timing:
            env["SUBLIFT_STAGE_TIMING"] = "1"
        else:
            env.pop("SUBLIFT_STAGE_TIMING", None)
        import time

        started = time.perf_counter()
        completed = subprocess.run(
            command,
            capture_output=True,
            text=True,
            env=env,
            check=False,
        )
        wall = time.perf_counter() - started
        if completed.returncode != 0:
            detail = (completed.stderr or completed.stdout).strip()
            raise RuntimeError(f"Native extract failed: {detail}")
        srt_bytes = output.read_bytes() if output.exists() else b""
        srt_sha = hashlib.sha256(srt_bytes).hexdigest()
        srt_entries = srt_bytes.count(b"\n\n") if srt_bytes else 0
        if not stage_timing:
            return RunRecord(
                index=-1,
                wall_seconds=wall,
                core_wall_ms=0.0,
                job_wall_ms=0.0,
                coverage_pct=0.0,
                srt_sha256=srt_sha,
                srt_entries=srt_entries,
            ), completed.stderr
        payload = _parse_stage_line(completed.stderr)
        stages: dict[str, StageAggregate] = {}
        for name, entry in payload["stages"].items():
            stages[name] = StageAggregate(
                calls=int(entry["calls"]),
                total_ms=float(entry["total_ms"]),
            )
        record = RunRecord(
            index=-1,
            wall_seconds=wall,
            core_wall_ms=float(payload["core_wall_ms"]),
            job_wall_ms=float(payload["job_wall_ms"]),
            coverage_pct=float(payload["coverage_pct"]),
            srt_sha256=srt_sha,
            srt_entries=srt_entries,
            stages=stages,
        )
        return record, completed.stderr


def _median_stages(runs: list[RunRecord]) -> tuple[dict[str, StageAggregate], float, float, float]:
    names: set[str] = set()
    for run in runs:
        names.update(run.stages)
    medians: dict[str, StageAggregate] = {}
    for name in sorted(names):
        calls = [run.stages[name].calls for run in runs if name in run.stages]
        totals = [run.stages[name].total_ms for run in runs if name in run.stages]
        medians[name] = StageAggregate(
            calls=int(statistics.median(calls)),
            total_ms=float(statistics.median(totals)),
        )
    core_wall = float(statistics.median([run.core_wall_ms for run in runs]))
    job_wall = float(statistics.median([run.job_wall_ms for run in runs]))
    coverage = float(statistics.median([run.coverage_pct for run in runs]))
    return medians, core_wall, job_wall, coverage


def _markdown_table(
    label: str,
    medians: dict[str, StageAggregate],
    core_wall_ms: float,
    job_wall_ms: float,
    coverage_pct: float,
    runs: list[RunRecord],
    baseline_run: RunRecord,
) -> str:
    lines: list[str] = []
    lines.append(f"## Native 阶段归因（{label}，measured median）")
    lines.append("")
    lines.append(f"- core_wall median: {core_wall_ms:.1f} ms")
    lines.append(f"- job_wall median: {job_wall_ms:.1f} ms（含 plan/probe 与引擎创建）")
    lines.append(f"- coverage: {coverage_pct:.4f}%（顶层叶子 / core_wall）")
    lines.append(
        "- 输出 SRT sha256: "
        + ", ".join(sorted({run.srt_sha256[:16] for run in runs}))
        + f"（与计时关闭基线一致: {baseline_run.srt_sha256[:16]}）"
    )
    lines.append("")
    lines.append("| 阶段 | 父级 | calls | total_ms | % of core_wall | % of parent |")
    lines.append("|---|---|---:|---:|---:|---:|")

    def pct_of(value: float, total: float) -> str:
        return f"{value / total * 100.0:.2f}%" if total > 0 else "-"

    ordered = sorted(
        medians.items(),
        key=lambda kv: (STAGE_TREE.get(kv[0], ""), kv[0]),
    )
    for name, entry in ordered:
        parent = STAGE_TREE.get(name, "")
        parent_total = medians[parent].total_ms if parent in medians else core_wall_ms
        lines.append(
            f"| `{name}` | {parent or '—'} | {entry.calls} | {entry.total_ms:.1f} "
            f"| {pct_of(entry.total_ms, core_wall_ms)} | {pct_of(entry.total_ms, parent_total)} |"
        )
    lines.append("")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--video", required=True, type=Path)
    parser.add_argument("--engine", default="paddle")
    parser.add_argument("--fps", default=5.0, type=float)
    parser.add_argument("--script", default="cjk")
    parser.add_argument("--confidence", default=0.5, type=float)
    parser.add_argument("--warmup", default=1, type=int)
    parser.add_argument("--measured", default=3, type=int)
    parser.add_argument("--label", default="native_attribution")
    parser.add_argument(
        "--out-json",
        type=Path,
        default=REPO_ROOT / "debug" / "benchmark" / "native_attribution.json",
    )
    parser.add_argument(
        "--out-md",
        type=Path,
        default=REPO_ROOT / "debug" / "benchmark" / "native_attribution.md",
    )
    args = parser.parse_args(argv)

    if args.measured < 3:
        print("measured 必须 >= 3（median 协议）", file=sys.stderr)
        return 2
    cli = resolve_native_cli(REPO_ROOT)
    if cli is None:
        print("未找到 Native CLI（build/cpp/bin/sublift）", file=sys.stderr)
        return 2
    worker = resolve_worker_bin(REPO_ROOT)
    video = args.video if args.video.is_absolute() else REPO_ROOT / args.video
    if not video.is_file():
        print(f"视频不存在: {video}", file=sys.stderr)
        return 2

    for i in range(args.warmup):
        print(f"[warmup {i + 1}/{args.warmup}] {video.name} engine={args.engine} ...")
        _run_once(
            cli,
            worker,
            video,
            engine=args.engine,
            fps=args.fps,
            script=args.script,
            confidence=args.confidence,
            stage_timing=True,
            tag=f"warmup{i}",
        )

    runs: list[RunRecord] = []
    for i in range(args.measured):
        print(f"[measured {i + 1}/{args.measured}] {video.name} engine={args.engine} ...")
        record, _ = _run_once(
            cli,
            worker,
            video,
            engine=args.engine,
            fps=args.fps,
            script=args.script,
            confidence=args.confidence,
            stage_timing=True,
            tag=f"measured{i}",
        )
        record.index = i
        runs.append(record)

    baseline_run, _ = _run_once(
        cli,
        worker,
        video,
        engine=args.engine,
        fps=args.fps,
        script=args.script,
        confidence=args.confidence,
        stage_timing=False,
        tag="baseline",
    )

    hashes = {run.srt_sha256 for run in runs}
    if len(hashes) != 1:
        print(f"measured runs SRT hash 不一致: {hashes}", file=sys.stderr)
        return 1
    if baseline_run.srt_sha256 not in hashes:
        print(
            "计时开启与关闭的 SRT hash 不一致（插桩不是惰性的）: "
            f"{hashes} vs {baseline_run.srt_sha256}",
            file=sys.stderr,
        )
        return 1

    medians, core_wall_ms, job_wall_ms, coverage_pct = _median_stages(runs)
    report: dict[str, Any] = {
        "label": args.label,
        "video": str(video),
        "engine": args.engine,
        "fps": args.fps,
        "script": args.script,
        "confidence": args.confidence,
        "protocol": {
            "warmup": args.warmup,
            "measured": args.measured,
            "statistic": "median",
            "independent_processes": True,
        },
        "core_wall_ms": core_wall_ms,
        "job_wall_ms": job_wall_ms,
        "coverage_pct": coverage_pct,
        "srt_sha256": baseline_run.srt_sha256,
        "srt_entries": baseline_run.srt_entries,
        "stages": {
            name: {"calls": entry.calls, "total_ms": entry.total_ms}
            for name, entry in medians.items()
        },
        "runs": [
            {
                "index": run.index,
                "wall_seconds": run.wall_seconds,
                "core_wall_ms": run.core_wall_ms,
                "job_wall_ms": run.job_wall_ms,
                "coverage_pct": run.coverage_pct,
                "srt_sha256": run.srt_sha256,
            }
            for run in runs
        ],
    }
    args.out_json.parent.mkdir(parents=True, exist_ok=True)
    args.out_json.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    table = _markdown_table(
        args.label,
        medians,
        core_wall_ms,
        job_wall_ms,
        coverage_pct,
        runs,
        baseline_run,
    )
    args.out_md.parent.mkdir(parents=True, exist_ok=True)
    args.out_md.write_text(table, encoding="utf-8")
    print(table)
    print(f"JSON: {args.out_json}")
    print(f"Markdown: {args.out_md}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
