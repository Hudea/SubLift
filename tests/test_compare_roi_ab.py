"""feat-039：compare_roi_ab 硬门脚本单测（合成 fixture，不跑 Vision）。"""

from __future__ import annotations

from typing import Any

from scripts.compare_roi_ab import evaluate

_HASH = "b2d35c1e25f156e1"
_COMMIT = "fb4a27a8c81316878701e2f2ea8222910d2b1021"
_FRAME_COUNT = 1271
_FULL_RAW = 7_906_636_800
_ROI_RAW = 636_923_520
_ROI_BPF = 501_120


def _quality_run(run_index: int = 1) -> dict[str, Any]:
    return {
        "run_index": run_index,
        "detection_hash": _HASH,
        "all_pass": True,
        "metrics": {
            "timing_f1": 0.9767,
            "timing_precision": 0.9882,
            "usable_subtitle_recall": 0.9195,
            "cer_macro": 0.032,
            "text_noise": 0,
            "text_empty": 0,
        },
    }


def _agent(
    *,
    mode: str,
    raw_bytes_per_frame: int,
    raw_output_bytes: int,
    detection_hash: str = _HASH,
    core_wall_ms: float = 10_000.0,
    materialize_ms: float = 1_600.0,
    peak_rss: float = 300_000_000.0,
    realtime_factor: float = 25.0,
    pipeline_crop_count: int = 0,
    stage_coverage_pct: float = 99.5,
    quality_runs: list[dict[str, Any]] | None = None,
    git_dirty: bool = False,
    git_commit: str = _COMMIT,
    frame_count: int = _FRAME_COUNT,
) -> dict[str, Any]:
    runs = quality_runs if quality_runs is not None else [
        _quality_run(1),
        _quality_run(2),
        _quality_run(3),
    ]
    thr = {
        "frame_count": frame_count,
        "output_mode": mode,
        "raw_bytes_per_frame": raw_bytes_per_frame,
        "raw_output_bytes": raw_output_bytes,
        "pipeline_crop_count": pipeline_crop_count,
        "stage_coverage_pct": stage_coverage_pct,
    }
    return {
        "performance": {
            "throughput": thr,
            "environment": {
                "git_commit": git_commit,
                "git_dirty": git_dirty,
            },
            "aggregate": {
                "core_wall_ms": {"median": core_wall_ms},
                "python_peak_rss_bytes": {"median": peak_rss},
                "realtime_factor": {"median": realtime_factor},
                "stage_total_ms": {
                    "frame_materialize": {"median": materialize_ms},
                },
            },
            "runs": [{"throughput": thr}],
            "quality": {
                "reference_detection_hash": detection_hash,
                "detections_consistent": True,
                "all_runs_pass": True,
                "runs": runs,
            },
        }
    }


def _passing_pair() -> tuple[dict[str, Any], dict[str, Any]]:
    full = _agent(
        mode="full_rgb",
        raw_bytes_per_frame=1920 * 1080 * 3,
        raw_output_bytes=_FULL_RAW,
        core_wall_ms=10_460.0,
        materialize_ms=1_640.0,
        peak_rss=360_000_000.0,
        realtime_factor=24.3,
    )
    roi = _agent(
        mode="roi_rgb",
        raw_bytes_per_frame=_ROI_BPF,
        raw_output_bytes=_ROI_RAW,
        core_wall_ms=8_702.0,
        materialize_ms=220.0,
        peak_rss=305_000_000.0,
        realtime_factor=29.2,
        pipeline_crop_count=0,
    )
    return full, roi


class TestCompareRoiAbHardGates:
    def test_passing_pair_hard_gates_pass(self) -> None:
        full, roi = _passing_pair()
        report = evaluate(full, roi)
        assert report["hard_gates_pass"] is True
        failed = [c["name"] for c in report["checks"] if not c["pass"]]
        assert failed == []

    def test_detection_hash_mismatch_fails(self) -> None:
        full, roi = _passing_pair()
        roi["performance"]["quality"]["reference_detection_hash"] = "deadbeefdeadbeef"
        report = evaluate(full, roi)
        assert report["hard_gates_pass"] is False
        by_name = {c["name"]: c for c in report["checks"]}
        assert by_name["detection_hash_equal"]["pass"] is False

    def test_raw_bytes_ratio_wrong_fails(self) -> None:
        full, roi = _passing_pair()
        roi["performance"]["throughput"]["raw_output_bytes"] = _FULL_RAW  # 假装没裁
        roi["performance"]["runs"][0]["throughput"]["raw_output_bytes"] = _FULL_RAW
        report = evaluate(full, roi)
        assert report["hard_gates_pass"] is False
        by_name = {c["name"]: c for c in report["checks"]}
        assert by_name["raw_output_bytes_ratio"]["pass"] is False

    def test_pipeline_crop_nonzero_fails(self) -> None:
        full, roi = _passing_pair()
        roi["performance"]["throughput"]["pipeline_crop_count"] = 12
        roi["performance"]["runs"][0]["throughput"]["pipeline_crop_count"] = 12
        report = evaluate(full, roi)
        assert report["hard_gates_pass"] is False
        by_name = {c["name"]: c for c in report["checks"]}
        assert by_name["roi_pipeline_crop_zero"]["pass"] is False

    def test_soft_wall_miss_does_not_block_hard(self) -> None:
        """软目标 wall≤90% 失败时，硬门 core_wall≤105% 仍可通过。"""
        full, roi = _passing_pair()
        # ratio = 0.95 → 硬门 1.05 过，软门 0.90 不过
        full["performance"]["aggregate"]["core_wall_ms"]["median"] = 10_000.0
        roi["performance"]["aggregate"]["core_wall_ms"]["median"] = 9_500.0
        report = evaluate(full, roi)
        assert report["hard_gates_pass"] is True
        soft = report["soft_goals"]
        assert soft["core_wall_le_90pct_full"]["pass"] is False

    def test_dirty_commit_fails_same_clean_commit(self) -> None:
        full, roi = _passing_pair()
        full["performance"]["environment"]["git_dirty"] = True
        report = evaluate(full, roi)
        assert report["hard_gates_pass"] is False
        by_name = {c["name"]: c for c in report["checks"]}
        assert by_name["same_clean_commit"]["pass"] is False

    def test_quality_gate_fail_on_one_run(self) -> None:
        full, roi = _passing_pair()
        bad = _quality_run(2)
        bad["metrics"]["timing_f1"] = 0.50  # 低于 0.952
        roi["performance"]["quality"]["runs"] = [
            _quality_run(1),
            bad,
            _quality_run(3),
        ]
        report = evaluate(full, roi)
        assert report["hard_gates_pass"] is False
        by_name = {c["name"]: c for c in report["checks"]}
        assert by_name["roi_quality_all_runs"]["pass"] is False
