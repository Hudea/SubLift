#!/usr/bin/env python3
"""Run the Phase 6.8 Paddle Det parity hard gate.

This is an explicit live gate: it loads the frozen PP-OCRv6 models, replays
the shared fixture manifest through Python RapidOCR and C++ Native, and checks
actual tensor binaries, probability maps, boxes, scores, and empty semantics.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

import cv2
import numpy as np

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from scripts.parity.dump_paddle_stages import build_live_report  # noqa: E402
from scripts.parity.paddle_stage_trace import (  # noqa: E402
    resolve_cpp_trace_bin,
    sha256_file,
)

TENSOR_MAX_ABS = 1e-5
PROBABILITY_MAX_ABS = 1e-5
PROBABILITY_CROSS_BUILD_MAX_ABS = 2.5e-5
BOX_COORD_MAX_ABS = 1.0
BOX_SCORE_MAX_ABS = 1e-4
BOX_IOU_THRESHOLD = 0.95


def _stage(trace: dict[str, Any], name: str) -> dict[str, Any]:
    value = trace["stages"][name]
    if not isinstance(value, dict):
        raise RuntimeError(f"invalid trace stage {name}")
    return value


def _read_raw(
    summary: dict[str, Any],
    runtime_dir: Path,
) -> np.ndarray:
    raw_file = summary.get("raw_file")
    if not isinstance(raw_file, str):
        raise RuntimeError("live trace tensor is missing raw_file")
    dtype_name = summary["dtype"]
    if dtype_name != "float32-le":
        raise RuntimeError(f"unexpected Det dtype: {dtype_name}")
    value = np.fromfile(runtime_dir / raw_file, dtype="<f4")
    expected_count = int(summary["count"])
    if value.size != expected_count:
        raise RuntimeError(
            f"raw tensor count mismatch: {value.size} != {expected_count}"
        )
    return value


def _array_error(
    oracle_summary: dict[str, Any],
    candidate_summary: dict[str, Any],
    oracle_dir: Path,
    candidate_dir: Path,
) -> tuple[bool, float | None]:
    if oracle_summary["shape"] != candidate_summary["shape"]:
        return False, None
    oracle = _read_raw(oracle_summary, oracle_dir)
    candidate = _read_raw(candidate_summary, candidate_dir)
    if oracle.shape != candidate.shape:
        return False, None
    if oracle.size == 0:
        return True, 0.0
    return True, float(np.max(np.abs(oracle - candidate)))


def _quad_array(quad: dict[str, Any]) -> np.ndarray:
    value = np.asarray(quad["points"], dtype=np.float32)
    if value.shape != (4, 2):
        raise RuntimeError(f"invalid Paddle quad shape: {value.shape}")
    return value


def _quad_iou(lhs: np.ndarray, rhs: np.ndarray) -> float:
    lhs_hull = cv2.convexHull(lhs)
    rhs_hull = cv2.convexHull(rhs)
    lhs_area = float(cv2.contourArea(lhs_hull))
    rhs_area = float(cv2.contourArea(rhs_hull))
    intersection_area, _intersection = cv2.intersectConvexConvex(
        lhs_hull, rhs_hull
    )
    union = lhs_area + rhs_area - float(intersection_area)
    return float(intersection_area) / union if union > 0.0 else 0.0


def _match_quads(
    oracle: list[dict[str, Any]],
    candidate: list[dict[str, Any]],
) -> dict[str, Any]:
    pairs: list[tuple[float, int, int]] = []
    for oracle_index, oracle_quad in enumerate(oracle):
        for candidate_index, candidate_quad in enumerate(candidate):
            pairs.append(
                (
                    _quad_iou(
                        _quad_array(oracle_quad),
                        _quad_array(candidate_quad),
                    ),
                    oracle_index,
                    candidate_index,
                )
            )
    pairs.sort(reverse=True)
    used_oracle: set[int] = set()
    used_candidate: set[int] = set()
    matches: list[dict[str, Any]] = []
    for iou, oracle_index, candidate_index in pairs:
        if iou < BOX_IOU_THRESHOLD:
            break
        if oracle_index in used_oracle or candidate_index in used_candidate:
            continue
        used_oracle.add(oracle_index)
        used_candidate.add(candidate_index)
        oracle_quad = oracle[oracle_index]
        candidate_quad = candidate[candidate_index]
        coord_error = float(
            np.max(
                np.abs(
                    _quad_array(oracle_quad) - _quad_array(candidate_quad)
                )
            )
        )
        score_error = abs(
            float(oracle_quad["score"]) - float(candidate_quad["score"])
        )
        matches.append(
            {
                "oracle_index": oracle_index,
                "candidate_index": candidate_index,
                "iou": iou,
                "coord_max_abs": coord_error,
                "score_abs": score_error,
            }
        )

    precision = len(matches) / len(candidate) if candidate else float(not oracle)
    recall = len(matches) / len(oracle) if oracle else float(not candidate)
    return {
        "oracle_count": len(oracle),
        "candidate_count": len(candidate),
        "match_count": len(matches),
        "precision_at_iou_0_95": precision,
        "recall_at_iou_0_95": recall,
        "matches": matches,
    }


def evaluate_det_report(
    live: dict[str, Any],
    raw_root: Path,
    *,
    candidate_ort_sha256: str,
) -> dict[str, Any]:
    cases: list[dict[str, Any]] = []
    passed = True
    aggregate_tensor_error = 0.0
    aggregate_probability_error = 0.0
    aggregate_coord_error = 0.0
    aggregate_score_error = 0.0
    oracle_ort_sha256 = str(
        live["runtime_versions"]["onnxruntime_library_sha256"]
    )
    same_ort_binary = candidate_ort_sha256 == oracle_ort_sha256
    probability_threshold = (
        PROBABILITY_MAX_ABS
        if same_ort_binary
        else PROBABILITY_CROSS_BUILD_MAX_ABS
    )

    for case in live["cases"]:
        case_id = str(case["case_id"])
        oracle = case["oracle"]
        candidate = case["candidate"]
        oracle_dir = raw_root / case_id / "python"
        candidate_dir = raw_root / case_id / "cpp"

        oracle_tensor = _stage(oracle, "2_det_preprocess")["tensor"]
        candidate_tensor = _stage(candidate, "2_det_preprocess")["tensor"]
        tensor_shape_equal, tensor_error = _array_error(
            oracle_tensor,
            candidate_tensor,
            oracle_dir,
            candidate_dir,
        )
        oracle_probability = _stage(oracle, "3_det_infer")["probability_map"]
        candidate_probability = _stage(
            candidate, "3_det_infer"
        )["probability_map"]
        probability_shape_equal, probability_error = _array_error(
            oracle_probability,
            candidate_probability,
            oracle_dir,
            candidate_dir,
        )

        oracle_quads = _stage(oracle, "4_det_postprocess")["quads"]
        candidate_quads = _stage(candidate, "4_det_postprocess")["quads"]
        boxes = _match_quads(oracle_quads, candidate_quads)
        match_coord_error = max(
            (float(item["coord_max_abs"]) for item in boxes["matches"]),
            default=0.0,
        )
        match_score_error = max(
            (float(item["score_abs"]) for item in boxes["matches"]),
            default=0.0,
        )

        empty_ok = True
        if case_id == "empty":
            empty_ok = (
                int(candidate["counts"]["det_boxes"]) == 0
                and int(candidate["counts"]["rec_calls"]) == 0
                and _stage(candidate, "10_output")["line_count"] == 0
            )

        case_passed = (
            tensor_shape_equal
            and tensor_error is not None
            and tensor_error <= TENSOR_MAX_ABS
            and probability_shape_equal
            and probability_error is not None
            and probability_error <= probability_threshold
            and boxes["precision_at_iou_0_95"] == 1.0
            and boxes["recall_at_iou_0_95"] == 1.0
            and match_coord_error <= BOX_COORD_MAX_ABS
            and match_score_error <= BOX_SCORE_MAX_ABS
            and empty_ok
        )
        passed = passed and case_passed
        aggregate_tensor_error = max(
            aggregate_tensor_error, tensor_error or 0.0
        )
        aggregate_probability_error = max(
            aggregate_probability_error, probability_error or 0.0
        )
        aggregate_coord_error = max(
            aggregate_coord_error, match_coord_error
        )
        aggregate_score_error = max(
            aggregate_score_error, match_score_error
        )
        cases.append(
            {
                "case_id": case_id,
                "passed": case_passed,
                "det_tensor_shape_equal": tensor_shape_equal,
                "det_tensor_max_abs": tensor_error,
                "probability_shape_equal": probability_shape_equal,
                "probability_max_abs": probability_error,
                "boxes": boxes,
                "empty_semantics_ok": empty_ok,
            }
        )

    return {
        "schema_version": 1,
        "kind": "paddle_det_parity_gate",
        "passed": passed,
        "thresholds": {
            "det_tensor_max_abs": TENSOR_MAX_ABS,
            "probability_max_abs_same_ort_binary": PROBABILITY_MAX_ABS,
            "probability_max_abs_cross_build": PROBABILITY_CROSS_BUILD_MAX_ABS,
            "box_iou": BOX_IOU_THRESHOLD,
            "box_coord_max_abs": BOX_COORD_MAX_ABS,
            "box_score_abs": BOX_SCORE_MAX_ABS,
        },
        "aggregate": {
            "case_count": len(cases),
            "det_tensor_max_abs": aggregate_tensor_error,
            "probability_max_abs": aggregate_probability_error,
            "box_coord_max_abs": aggregate_coord_error,
            "box_score_abs": aggregate_score_error,
        },
        "ort_binary_fingerprint": {
            "oracle_sha256": oracle_ort_sha256,
            "candidate_sha256": candidate_ort_sha256,
            "same_binary": same_ort_binary,
            "active_probability_threshold": probability_threshold,
            "note": (
                "Exact ORT binary gate"
                if same_ort_binary
                else "Same ORT version/provider, different binary build; "
                "cross-build tail tolerance is active"
            ),
        },
        "fingerprint": live["fingerprint"],
        "runtime_versions": live["runtime_versions"],
        "observed_candidate_commit": live.get("observed_candidate_commit"),
        "cases": cases,
    }


def _detect_candidate_ort_library() -> Path | None:
    trace_bin = resolve_cpp_trace_bin(REPO_ROOT)
    if trace_bin is None:
        raise RuntimeError("C++ Paddle trace executable is unavailable")
    if sys.platform == "darwin":
        command = ["otool", "-L", str(trace_bin)]
    elif sys.platform.startswith("linux"):
        command = ["ldd", str(trace_bin)]
    else:
        return None
    linked = subprocess.run(
        command,
        capture_output=True,
        text=True,
        check=True,
    )
    for line in linked.stdout.splitlines():
        fields = line.strip().replace("=>", " ").split()
        for field in fields:
            path = Path(field)
            if "onnxruntime" in field.lower() and path.is_file():
                return path
    return None


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Check live Python/C++ PP-OCR Det parity"
    )
    parser.add_argument("--check", action="store_true", help="fail on gate miss")
    parser.add_argument("--report-out", type=Path, default=None)
    parser.add_argument(
        "--candidate-ort-library",
        type=Path,
        default=None,
        help="explicit C++ ORT dylib path for strict binary fingerprinting",
    )
    args = parser.parse_args()

    candidate_ort_library = args.candidate_ort_library
    if candidate_ort_library is None:
        candidate_ort_library = _detect_candidate_ort_library()
    if candidate_ort_library is None or not candidate_ort_library.is_file():
        raise RuntimeError(
            "cannot fingerprint the C++ ONNX Runtime library; pass "
            "--candidate-ort-library"
        )
    candidate_ort_sha256 = sha256_file(candidate_ort_library)

    with tempfile.TemporaryDirectory(prefix="sublift-paddle-det-") as tmp:
        raw_root = Path(tmp)
        live = build_live_report(
            raw_dir=raw_root,
            report_with_timings=True,
        )
        report = evaluate_det_report(
            live,
            raw_root,
            candidate_ort_sha256=candidate_ort_sha256,
        )

    formatted = json.dumps(report, indent=2, ensure_ascii=False) + "\n"
    if args.report_out is not None:
        args.report_out.parent.mkdir(parents=True, exist_ok=True)
        args.report_out.write_text(formatted, encoding="utf-8")
    print(formatted, end="")
    if args.check and not report["passed"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
