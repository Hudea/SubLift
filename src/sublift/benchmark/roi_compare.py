"""Compare same-commit full vs ROI benchmark reports against feat-039 gates.

Usage:
    uv run sublift-benchmark compare-roi full.agent.json roi.agent.json
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

# feat-039 hard gates (docs/plans/phase4-roi-data-path.md)
_ROI_RAW_BYTES_PER_FRAME = 501_120  # 1920 * 87 * 3
_ROI_AREA_RATIO = 87 / 1080
_QUALITY_TIMING_F1 = 0.952
_QUALITY_TIMING_PRECISION = 0.988
_QUALITY_USABLE = 0.851
_QUALITY_CER_MACRO = 0.066
_QUALITY_NOISE_MAX = 2
_QUALITY_EMPTY_MAX = 1
_MATERIALIZE_RATIO_MAX = 0.25
_CORE_WALL_RATIO_MAX = 1.05
_RSS_RATIO_MAX = 1.05
_STAGE_COVERAGE_MIN = 99.0
# Soft (non-blocking) goals
_SOFT_CORE_WALL_RATIO = 0.90
_SOFT_REALTIME_FACTOR_MULT = 1.10


def _load(path: Path) -> dict[str, Any]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise TypeError(f"agent JSON 顶层必须是 object: {path}")
    return data


def _median(vals: list[float]) -> float | None:
    if not vals:
        return None
    ordered = sorted(vals)
    mid = len(ordered) // 2
    if len(ordered) % 2:
        return ordered[mid]
    return (ordered[mid - 1] + ordered[mid]) / 2.0


def _stage_total_median(perf: dict[str, Any], stage: str) -> float | None:
    agg = perf.get("aggregate") or {}
    stage_map = agg.get("stage_total_ms") or {}
    entry = stage_map.get(stage) or {}
    med = entry.get("median")
    return float(med) if isinstance(med, int | float) else None


def _agg_median(perf: dict[str, Any], key: str) -> float | None:
    agg = perf.get("aggregate") or {}
    entry = agg.get(key) or {}
    med = entry.get("median")
    return float(med) if isinstance(med, int | float) else None


def _run_throughputs(perf: dict[str, Any]) -> list[dict[str, Any]]:
    runs = perf.get("runs") or []
    out: list[dict[str, Any]] = []
    for run in runs:
        thr = run.get("throughput") if isinstance(run, dict) else None
        if isinstance(thr, dict):
            out.append(thr)
    return out


def evaluate(full: dict[str, Any], roi: dict[str, Any]) -> dict[str, Any]:
    """Evaluate hard gates; return structured report."""
    full_perf = full.get("performance") or {}
    roi_perf = roi.get("performance") or {}
    full_thr = full_perf.get("throughput") or {}
    roi_thr = roi_perf.get("throughput") or {}
    full_q = full_perf.get("quality") or {}
    roi_q = roi_perf.get("quality") or {}

    checks: list[dict[str, Any]] = []

    def add(name: str, passed: bool, detail: dict[str, Any]) -> None:
        checks.append({"name": name, "pass": passed, **detail})

    # --- sampling / metadata ---
    full_frames = int(full_thr.get("frame_count") or 0)
    roi_frames = int(roi_thr.get("frame_count") or 0)
    add(
        "frame_count_equal",
        full_frames == roi_frames and full_frames > 0,
        {"full": full_frames, "roi": roi_frames},
    )

    add(
        "output_mode",
        full_thr.get("output_mode") == "full_rgb" and roi_thr.get("output_mode") == "roi_rgb",
        {
            "full_mode": full_thr.get("output_mode"),
            "roi_mode": roi_thr.get("output_mode"),
        },
    )

    # --- pixel transfer ---
    roi_bpf = int(roi_thr.get("raw_bytes_per_frame") or 0)
    add(
        "roi_raw_bytes_per_frame",
        roi_bpf == _ROI_RAW_BYTES_PER_FRAME,
        {"actual": roi_bpf, "expected": _ROI_RAW_BYTES_PER_FRAME},
    )

    full_raw = float(full_thr.get("raw_output_bytes") or 0)
    roi_raw = float(roi_thr.get("raw_output_bytes") or 0)
    ratio = (roi_raw / full_raw) if full_raw > 0 else None
    add(
        "raw_output_bytes_ratio",
        ratio is not None and abs(ratio - _ROI_AREA_RATIO) < 1e-6,
        {
            "full_bytes": full_raw,
            "roi_bytes": roi_raw,
            "ratio": ratio,
            "expected_ratio": _ROI_AREA_RATIO,
            "expected_roi_bytes": full_raw * _ROI_AREA_RATIO if full_raw else None,
        },
    )

    # --- detection hash ---
    full_hash = full_q.get("reference_detection_hash")
    roi_hash = roi_q.get("reference_detection_hash")
    full_consistent = bool(full_q.get("detections_consistent"))
    roi_consistent = bool(roi_q.get("detections_consistent"))
    full_env_raw = full_perf.get("environment")
    roi_env_raw = roi_perf.get("environment")
    full_env = full_env_raw if isinstance(full_env_raw, dict) else {}
    roi_env = roi_env_raw if isinstance(roi_env_raw, dict) else {}
    same_commit = (
        full_env.get("git_commit")
        and full_env.get("git_commit") == roi_env.get("git_commit")
        and full_env.get("git_dirty") is False
        and roi_env.get("git_dirty") is False
    )
    add(
        "same_clean_commit",
        bool(same_commit),
        {
            "full_commit": full_env.get("git_commit"),
            "roi_commit": roi_env.get("git_commit"),
            "full_dirty": full_env.get("git_dirty"),
            "roi_dirty": roi_env.get("git_dirty"),
        },
    )

    add(
        "detection_hash_equal",
        bool(full_hash) and full_hash == roi_hash and full_consistent and roi_consistent,
        {
            "full_hash": full_hash,
            "roi_hash": roi_hash,
            "full_consistent": full_consistent,
            "roi_consistent": roi_consistent,
            "historical_anchor": "b2d35c1e25f156e1",
            "matches_historical_anchor": full_hash == "b2d35c1e25f156e1",
        },
    )

    # --- quality gates on all ROI measured runs ---
    roi_runs = roi_q.get("runs") or []
    quality_detail: list[dict[str, Any]] = []

    def _num(src: dict[str, Any], key: str, default: float) -> float:
        val = src.get(key)
        return float(val) if isinstance(val, int | float) else default

    independent_all_pass = len(roi_runs) >= 3
    for run in roi_runs:
        metrics = run.get("metrics") if isinstance(run.get("metrics"), dict) else {}
        run_checks = {
            "timing_f1": _num(metrics, "timing_f1", 0.0) >= _QUALITY_TIMING_F1,
            "timing_precision": _num(metrics, "timing_precision", 0.0) >= _QUALITY_TIMING_PRECISION,
            "usable": _num(metrics, "usable_subtitle_recall", 0.0) >= _QUALITY_USABLE,
            "cer_macro": _num(metrics, "cer_macro", 1.0) <= _QUALITY_CER_MACRO,
            "noise": int(_num(metrics, "text_noise", 99)) <= _QUALITY_NOISE_MAX,
            "empty": int(_num(metrics, "text_empty", 99)) <= _QUALITY_EMPTY_MAX,
        }
        metrics_pass = all(run_checks.values())
        if not metrics_pass:
            independent_all_pass = False
        quality_detail.append(
            {
                "run_index": run.get("run_index"),
                "detection_hash": run.get("detection_hash"),
                "all_pass_flag": run.get("all_pass"),
                "metrics_pass": metrics_pass,
                "checks": run_checks,
                "metrics": metrics,
            }
        )
    # 以独立 metrics 复核为真源，不单信 agent 的 all_pass 标志
    add(
        "roi_quality_all_runs",
        independent_all_pass,
        {
            "runs": quality_detail,
            "agent_all_runs_pass": bool(roi_q.get("all_runs_pass")),
        },
    )

    # --- materialize cost ---
    full_mat = _stage_total_median(full_perf, "frame_materialize")
    roi_mat = _stage_total_median(roi_perf, "frame_materialize")
    mat_ratio = None
    if full_mat is not None and full_mat > 0 and roi_mat is not None:
        mat_ratio = roi_mat / full_mat
    add(
        "frame_materialize_ratio",
        mat_ratio is not None and mat_ratio <= _MATERIALIZE_RATIO_MAX,
        {
            "full_median_ms": full_mat,
            "roi_median_ms": roi_mat,
            "ratio": mat_ratio,
            "max_ratio": _MATERIALIZE_RATIO_MAX,
        },
    )

    # --- e2e non-regression ---
    full_wall = _agg_median(full_perf, "core_wall_ms")
    roi_wall = _agg_median(roi_perf, "core_wall_ms")
    wall_ratio = (
        (roi_wall / full_wall)
        if full_wall is not None and full_wall > 0 and roi_wall is not None
        else None
    )
    add(
        "core_wall_ratio",
        wall_ratio is not None and wall_ratio <= _CORE_WALL_RATIO_MAX,
        {
            "full_median_ms": full_wall,
            "roi_median_ms": roi_wall,
            "ratio": wall_ratio,
            "max_ratio": _CORE_WALL_RATIO_MAX,
        },
    )

    full_rss = _agg_median(full_perf, "python_peak_rss_bytes")
    roi_rss = _agg_median(roi_perf, "python_peak_rss_bytes")
    rss_ratio = (
        (roi_rss / full_rss)
        if full_rss is not None and full_rss > 0 and roi_rss is not None
        else None
    )
    add(
        "peak_rss_ratio",
        rss_ratio is not None and rss_ratio <= _RSS_RATIO_MAX,
        {
            "full_median_bytes": full_rss,
            "roi_median_bytes": roi_rss,
            "ratio": rss_ratio,
            "max_ratio": _RSS_RATIO_MAX,
        },
    )

    # stage coverage: all ROI measured runs
    roi_coverages: list[float] = []
    for t in _run_throughputs(roi_perf):
        cov = t.get("stage_coverage_pct")
        if isinstance(cov, int | float):
            roi_coverages.append(float(cov))
    add(
        "stage_coverage",
        bool(roi_coverages) and all(c >= _STAGE_COVERAGE_MIN for c in roi_coverages),
        {"roi_coverages": roi_coverages, "min": _STAGE_COVERAGE_MIN},
    )

    # pipeline crop invariant
    # 0 is a valid count — do not use `or` defaults that treat 0 as missing
    crop_counts: list[int] = []
    for t in _run_throughputs(roi_perf):
        if "pipeline_crop_count" in t and t["pipeline_crop_count"] is not None:
            crop_counts.append(int(t["pipeline_crop_count"]))
    # fallback: top-level throughput / counters (aggregate payload)
    if not crop_counts:
        top = roi_thr.get("pipeline_crop_count")
        if top is None and isinstance(roi_perf.get("counters"), dict):
            top = roi_perf["counters"].get("pipeline_crop_count")
        if top is not None:
            crop_counts = [int(top)]
    add(
        "roi_pipeline_crop_zero",
        bool(crop_counts) and all(c == 0 for c in crop_counts),
        {"pipeline_crop_counts": crop_counts},
    )

    # soft goals
    full_rt = _agg_median(full_perf, "realtime_factor")
    roi_rt = _agg_median(roi_perf, "realtime_factor")
    soft = {
        "core_wall_le_90pct_full": {
            "pass": wall_ratio is not None and wall_ratio <= _SOFT_CORE_WALL_RATIO,
            "ratio": wall_ratio,
            "target": _SOFT_CORE_WALL_RATIO,
        },
        "realtime_factor_ge_1_10x_full": {
            "pass": (
                full_rt is not None
                and roi_rt is not None
                and full_rt > 0
                and (roi_rt / full_rt) >= _SOFT_REALTIME_FACTOR_MULT
            ),
            "full": full_rt,
            "roi": roi_rt,
            "ratio": (roi_rt / full_rt) if full_rt and roi_rt else None,
            "target_mult": _SOFT_REALTIME_FACTOR_MULT,
        },
    }

    hard_pass = all(bool(c["pass"]) for c in checks)
    return {
        "hard_gates_pass": hard_pass,
        "checks": checks,
        "soft_goals": soft,
        "environment": {
            "full": full_perf.get("environment"),
            "roi": roi_perf.get("environment"),
            "note": "full/roi comparison is same-machine same-commit only",
        },
        "summary": {
            "full_hash": full_hash,
            "roi_hash": roi_hash,
            "full_core_wall_ms_median": full_wall,
            "roi_core_wall_ms_median": roi_wall,
            "wall_ratio": wall_ratio,
            "full_raw_bytes": full_raw,
            "roi_raw_bytes": roi_raw,
            "raw_ratio": ratio,
            "full_materialize_ms_median": full_mat,
            "roi_materialize_ms_median": roi_mat,
            "materialize_ratio": mat_ratio,
        },
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="feat-039 full/roi hard-gate compare")
    parser.add_argument("full_agent_json", type=Path)
    parser.add_argument("roi_agent_json", type=Path)
    parser.add_argument("--out", type=Path, default=None)
    args = parser.parse_args(argv)

    full = _load(args.full_agent_json)
    roi = _load(args.roi_agent_json)
    report = evaluate(full, roi)
    report["inputs"] = {
        "full": str(args.full_agent_json),
        "roi": str(args.roi_agent_json),
    }

    text = json.dumps(report, ensure_ascii=False, indent=2)
    if args.out is not None:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(text + "\n", encoding="utf-8")
        print(f"wrote {args.out}")

    print(text)
    if not report["hard_gates_pass"]:
        failed = [c["name"] for c in report["checks"] if not c["pass"]]
        print(f"HARD GATES FAILED: {failed}", file=sys.stderr)
        return 1
    print("HARD GATES PASSED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
