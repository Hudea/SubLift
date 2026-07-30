#!/usr/bin/env python3
"""Run the Phase 6.8 crop/Cls/Rec/final-output parity hard gate.

The live end-to-end replay checks user-visible output after the independent
Det gate. A second replay freezes the Python Det quads and feeds those exact
quads to C++, isolating perspective crop, direction classification, dynamic
Rec batching, right-zero padding, CTC decode, and final line mapping.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
import tempfile
from pathlib import Path
from typing import Any

import numpy as np
from PIL import Image

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from scripts.parity.dump_paddle_stages import (  # noqa: E402
    FIXTURES_DIR,
    build_live_report,
)
from scripts.parity.paddle_stage_trace import capture_cpp_trace  # noqa: E402

CROP_DIMENSION_MAX_ABS = 1
CROP_PIXEL_MEAN_ABS = 1.0
CROP_PIXEL_P99_ABS = 3.0
CLS_SCORE_MAX_ABS = 1e-4
REC_TENSOR_MAX_ABS = 1e-5
DECODE_CONFIDENCE_MAX_ABS = 1e-4
FINAL_AABB_MAX_ABS = 1
BATCH_SIZE = 6

JsonDict = dict[str, Any]


def _stage(trace: JsonDict, name: str) -> JsonDict:
    value = trace["stages"][name]
    if not isinstance(value, dict):
        raise RuntimeError(f"invalid trace stage {name}")
    return value


def _read_raw(
    summary: JsonDict,
    runtime_dir: Path,
    expected_dtype: str,
) -> np.ndarray:
    raw_file = summary.get("raw_file")
    if not isinstance(raw_file, str):
        raise RuntimeError("live trace value is missing raw_file")
    if summary.get("dtype") != expected_dtype:
        raise RuntimeError(
            f"unexpected dtype: {summary.get('dtype')} != {expected_dtype}"
        )
    dtype = np.dtype("<f4") if expected_dtype == "float32-le" else np.uint8
    value = np.fromfile(runtime_dir / raw_file, dtype=dtype)
    expected_count = int(summary["count"])
    if value.size != expected_count:
        raise RuntimeError(
            f"raw value count mismatch: {value.size} != {expected_count}"
        )
    return value.reshape(tuple(int(item) for item in summary["shape"]))


def _tensor_metrics(
    oracle_summaries: list[JsonDict],
    candidate_summaries: list[JsonDict],
    oracle_dir: Path,
    candidate_dir: Path,
) -> JsonDict:
    if len(oracle_summaries) != len(candidate_summaries):
        return {
            "batch_count_equal": False,
            "shape_equal": False,
            "max_abs": None,
        }
    max_abs = 0.0
    shape_equal = True
    for oracle_summary, candidate_summary in zip(
        oracle_summaries, candidate_summaries, strict=True
    ):
        if oracle_summary["shape"] != candidate_summary["shape"]:
            shape_equal = False
            continue
        oracle = _read_raw(oracle_summary, oracle_dir, "float32-le")
        candidate = _read_raw(candidate_summary, candidate_dir, "float32-le")
        if oracle.size:
            max_abs = max(
                max_abs,
                float(np.max(np.abs(oracle - candidate))),
            )
    return {
        "batch_count_equal": True,
        "shape_equal": shape_equal,
        "max_abs": max_abs if shape_equal else None,
    }


def _crop_metrics(
    oracle_crops: list[JsonDict],
    candidate_crops: list[JsonDict],
    oracle_dir: Path,
    candidate_dir: Path,
) -> JsonDict:
    if len(oracle_crops) != len(candidate_crops):
        return {
            "count_equal": False,
            "dimension_max_abs": None,
            "pixel_mean_abs": None,
            "pixel_p99_abs": None,
            "rotated_90_exact": False,
        }
    dimension_max_abs = 0
    pixel_differences: list[np.ndarray] = []
    rotated_90_exact = True
    for oracle_crop, candidate_crop in zip(
        oracle_crops, candidate_crops, strict=True
    ):
        oracle_summary = oracle_crop["image"]
        candidate_summary = candidate_crop["image"]
        oracle_shape = [int(item) for item in oracle_summary["shape"]]
        candidate_shape = [int(item) for item in candidate_summary["shape"]]
        dimension_max_abs = max(
            dimension_max_abs,
            abs(oracle_shape[0] - candidate_shape[0]),
            abs(oracle_shape[1] - candidate_shape[1]),
        )
        oracle = _read_raw(oracle_summary, oracle_dir, "uint8")
        candidate = _read_raw(candidate_summary, candidate_dir, "uint8")
        if oracle_summary.get("color_order") == "BGR":
            oracle = oracle[:, :, ::-1]
        height = min(oracle.shape[0], candidate.shape[0])
        width = min(oracle.shape[1], candidate.shape[1])
        difference = np.abs(
            oracle[:height, :width].astype(np.int16)
            - candidate[:height, :width].astype(np.int16)
        )
        pixel_differences.append(difference.reshape(-1))
        rotated_90_exact = rotated_90_exact and (
            bool(oracle_crop["rotated_90"])
            == bool(candidate_crop["rotated_90"])
        )
    all_differences = (
        np.concatenate(pixel_differences)
        if pixel_differences
        else np.asarray([], dtype=np.int16)
    )
    return {
        "count_equal": True,
        "dimension_max_abs": dimension_max_abs,
        "pixel_mean_abs": (
            float(np.mean(all_differences, dtype=np.float64))
            if all_differences.size
            else 0.0
        ),
        "pixel_p99_abs": (
            float(np.percentile(all_differences, 99))
            if all_differences.size
            else 0.0
        ),
        "rotated_90_exact": rotated_90_exact,
    }


def _classification_metrics(
    oracle_results: list[JsonDict],
    candidate_results: list[JsonDict],
) -> JsonDict:
    if len(oracle_results) != len(candidate_results):
        return {
            "count_equal": False,
            "labels_exact": False,
            "score_max_abs": None,
        }
    labels_exact = True
    score_max_abs = 0.0
    for oracle, candidate in zip(
        oracle_results, candidate_results, strict=True
    ):
        labels_exact = labels_exact and (
            str(oracle["label"]) == str(candidate["label"])
        )
        score_max_abs = max(
            score_max_abs,
            abs(float(oracle["score"]) - float(candidate["score"])),
        )
    return {
        "count_equal": True,
        "labels_exact": labels_exact,
        "score_max_abs": score_max_abs,
    }


def _decode_metrics(
    oracle_results: list[JsonDict],
    candidate_results: list[JsonDict],
) -> JsonDict:
    if len(oracle_results) != len(candidate_results):
        return {
            "count_equal": False,
            "tokens_exact": False,
            "text_exact": False,
            "confidence_max_abs": None,
        }
    tokens_exact = True
    text_exact = True
    confidence_max_abs = 0.0
    for oracle, candidate in zip(
        oracle_results, candidate_results, strict=True
    ):
        tokens_exact = tokens_exact and (
            oracle["tokens"] == candidate["tokens"]
        )
        text_exact = text_exact and (
            str(oracle["text"]) == str(candidate["text"])
        )
        confidence_max_abs = max(
            confidence_max_abs,
            abs(
                float(oracle["confidence"])
                - float(candidate["confidence"])
            ),
        )
    return {
        "count_equal": True,
        "tokens_exact": tokens_exact,
        "text_exact": text_exact,
        "confidence_max_abs": confidence_max_abs,
    }


def _final_metrics(oracle: JsonDict, candidate: JsonDict) -> JsonDict:
    oracle_lines = list(oracle["lines"])
    candidate_lines = list(candidate["lines"])
    line_count_equal = len(oracle_lines) == len(candidate_lines)
    text_exact = str(oracle["text"]) == str(candidate["text"])
    order_exact = line_count_equal
    aabb_max_abs = 0
    if line_count_equal:
        for oracle_line, candidate_line in zip(
            oracle_lines, candidate_lines, strict=True
        ):
            order_exact = order_exact and (
                str(oracle_line["text"]) == str(candidate_line["text"])
            )
            for key in ("x", "y", "width", "height"):
                aabb_max_abs = max(
                    aabb_max_abs,
                    abs(
                        int(oracle_line["box"][key])
                        - int(candidate_line["box"][key])
                    ),
                )
    return {
        "line_count_equal": line_count_equal,
        "text_exact": text_exact,
        "order_exact": order_exact,
        "aabb_max_abs": aabb_max_abs if line_count_equal else None,
    }


def _padding_is_exact_zero(
    candidate_trace: JsonDict,
    candidate_dir: Path,
) -> bool:
    crops = _stage(candidate_trace, "5_perspective_crop")["crops"]
    ratios = [
        float(crop["image"]["shape"][1])
        / float(max(1, int(crop["image"]["shape"][0])))
        for crop in crops
    ]
    indices = np.argsort(np.asarray(ratios))
    tensors = _stage(candidate_trace, "7_rec_preprocess")["tensors"]
    for batch_index, summary in enumerate(tensors):
        tensor = _read_raw(summary, candidate_dir, "float32-le")
        begin = batch_index * BATCH_SIZE
        batch_indices = indices[begin : begin + tensor.shape[0]]
        for item, crop_index in enumerate(batch_indices):
            shape = crops[int(crop_index)]["image"]["shape"]
            valid_width = min(
                tensor.shape[3],
                math.ceil(48.0 * float(shape[1]) / float(shape[0])),
            )
            if np.any(tensor[item, :, :, valid_width:] != 0.0):
                return False
    return True


def evaluate_rec_report(
    live: JsonDict,
    raw_root: Path,
    frozen_candidates: dict[str, JsonDict],
) -> JsonDict:
    cases: list[JsonDict] = []
    passed = True
    aggregate = {
        "crop_dimension_max_abs": 0,
        "crop_pixel_mean_abs": 0.0,
        "crop_pixel_p99_abs": 0.0,
        "cls_tensor_max_abs": 0.0,
        "cls_score_max_abs": 0.0,
        "rec_tensor_max_abs": 0.0,
        "decode_confidence_max_abs": 0.0,
        "final_aabb_max_abs": 0,
    }

    for case in live["cases"]:
        case_id = str(case["case_id"])
        oracle = case["oracle"]
        actual_candidate = case["candidate"]
        frozen_candidate = frozen_candidates[case_id]
        oracle_dir = raw_root / case_id / "python"
        frozen_dir = raw_root / case_id / "frozen_cpp"

        crops = _crop_metrics(
            _stage(oracle, "5_perspective_crop")["crops"],
            _stage(frozen_candidate, "5_perspective_crop")["crops"],
            oracle_dir,
            frozen_dir,
        )
        cls_tensors = _tensor_metrics(
            _stage(oracle, "6_cls").get("tensors", []),
            _stage(frozen_candidate, "6_cls").get("tensors", []),
            oracle_dir,
            frozen_dir,
        )
        classifications = _classification_metrics(
            _stage(oracle, "6_cls").get("results", []),
            _stage(frozen_candidate, "6_cls").get("results", []),
        )
        rec_tensors = _tensor_metrics(
            _stage(oracle, "7_rec_preprocess").get("tensors", []),
            _stage(frozen_candidate, "7_rec_preprocess").get("tensors", []),
            oracle_dir,
            frozen_dir,
        )
        decoded = _decode_metrics(
            _stage(oracle, "9_rec_decode").get("results", []),
            _stage(frozen_candidate, "9_rec_decode").get("results", []),
        )
        frozen_final = _final_metrics(
            _stage(oracle, "10_output"),
            _stage(frozen_candidate, "10_output"),
        )
        actual_final = _final_metrics(
            _stage(oracle, "10_output"),
            _stage(actual_candidate, "10_output"),
        )
        expected_batches = (
            math.ceil(
                int(_stage(oracle, "5_perspective_crop")["crop_count"])
                / BATCH_SIZE
            )
            if _stage(oracle, "5_perspective_crop")["crop_count"]
            else 0
        )
        batch_semantics_exact = (
            int(_stage(frozen_candidate, "6_cls").get("batch_count", 0))
            == expected_batches
            and int(
                _stage(frozen_candidate, "7_rec_preprocess").get(
                    "batch_count", 0
                )
            )
            == expected_batches
            and int(frozen_candidate["counts"]["cls_calls"])
            == expected_batches
            and int(frozen_candidate["counts"]["rec_calls"])
            == expected_batches
        )
        padding_exact_zero = _padding_is_exact_zero(
            frozen_candidate, frozen_dir
        )
        empty_semantics_ok = True
        if case_id == "empty":
            empty_semantics_ok = (
                int(frozen_candidate["counts"]["cls_calls"]) == 0
                and int(frozen_candidate["counts"]["rec_calls"]) == 0
                and int(_stage(frozen_candidate, "10_output")["line_count"])
                == 0
            )

        case_passed = (
            crops["count_equal"]
            and crops["dimension_max_abs"] is not None
            and crops["dimension_max_abs"] <= CROP_DIMENSION_MAX_ABS
            and crops["pixel_mean_abs"] is not None
            and crops["pixel_mean_abs"] <= CROP_PIXEL_MEAN_ABS
            and crops["pixel_p99_abs"] is not None
            and crops["pixel_p99_abs"] <= CROP_PIXEL_P99_ABS
            and crops["rotated_90_exact"]
            and cls_tensors["batch_count_equal"]
            and cls_tensors["shape_equal"]
            and cls_tensors["max_abs"] is not None
            and cls_tensors["max_abs"] <= REC_TENSOR_MAX_ABS
            and classifications["count_equal"]
            and classifications["labels_exact"]
            and classifications["score_max_abs"] is not None
            and classifications["score_max_abs"] <= CLS_SCORE_MAX_ABS
            and rec_tensors["batch_count_equal"]
            and rec_tensors["shape_equal"]
            and rec_tensors["max_abs"] is not None
            and rec_tensors["max_abs"] <= REC_TENSOR_MAX_ABS
            and padding_exact_zero
            and batch_semantics_exact
            and decoded["count_equal"]
            and decoded["tokens_exact"]
            and decoded["text_exact"]
            and decoded["confidence_max_abs"] is not None
            and decoded["confidence_max_abs"]
            <= DECODE_CONFIDENCE_MAX_ABS
            and frozen_final["line_count_equal"]
            and frozen_final["text_exact"]
            and frozen_final["order_exact"]
            and frozen_final["aabb_max_abs"] is not None
            and frozen_final["aabb_max_abs"] <= FINAL_AABB_MAX_ABS
            and actual_final["line_count_equal"]
            and actual_final["text_exact"]
            and actual_final["order_exact"]
            and actual_final["aabb_max_abs"] is not None
            and actual_final["aabb_max_abs"] <= FINAL_AABB_MAX_ABS
            and empty_semantics_ok
        )
        passed = passed and case_passed

        aggregate["crop_dimension_max_abs"] = max(
            aggregate["crop_dimension_max_abs"],
            int(crops["dimension_max_abs"] or 0),
        )
        aggregate["crop_pixel_mean_abs"] = max(
            aggregate["crop_pixel_mean_abs"],
            float(crops["pixel_mean_abs"] or 0.0),
        )
        aggregate["crop_pixel_p99_abs"] = max(
            aggregate["crop_pixel_p99_abs"],
            float(crops["pixel_p99_abs"] or 0.0),
        )
        aggregate["cls_tensor_max_abs"] = max(
            aggregate["cls_tensor_max_abs"],
            float(cls_tensors["max_abs"] or 0.0),
        )
        aggregate["cls_score_max_abs"] = max(
            aggregate["cls_score_max_abs"],
            float(classifications["score_max_abs"] or 0.0),
        )
        aggregate["rec_tensor_max_abs"] = max(
            aggregate["rec_tensor_max_abs"],
            float(rec_tensors["max_abs"] or 0.0),
        )
        aggregate["decode_confidence_max_abs"] = max(
            aggregate["decode_confidence_max_abs"],
            float(decoded["confidence_max_abs"] or 0.0),
        )
        aggregate["final_aabb_max_abs"] = max(
            aggregate["final_aabb_max_abs"],
            int(actual_final["aabb_max_abs"] or 0),
        )
        cases.append(
            {
                "case_id": case_id,
                "passed": case_passed,
                "frozen_det_operator_chain": {
                    "crops": crops,
                    "cls_tensors": cls_tensors,
                    "classifications": classifications,
                    "rec_tensors": rec_tensors,
                    "right_padding_exact_zero": padding_exact_zero,
                    "batch_semantics_exact": batch_semantics_exact,
                    "decoded": decoded,
                    "final": frozen_final,
                },
                "end_to_end_final": actual_final,
                "empty_semantics_ok": empty_semantics_ok,
            }
        )

    return {
        "schema_version": 1,
        "kind": "paddle_crop_cls_rec_parity_gate",
        "passed": passed,
        "thresholds": {
            "crop_dimension_max_abs": CROP_DIMENSION_MAX_ABS,
            "crop_pixel_mean_abs": CROP_PIXEL_MEAN_ABS,
            "crop_pixel_p99_abs": CROP_PIXEL_P99_ABS,
            "cls_score_max_abs": CLS_SCORE_MAX_ABS,
            "rec_tensor_max_abs": REC_TENSOR_MAX_ABS,
            "decode_confidence_max_abs": DECODE_CONFIDENCE_MAX_ABS,
            "final_aabb_max_abs": FINAL_AABB_MAX_ABS,
            "batch_size": BATCH_SIZE,
        },
        "aggregate": aggregate,
        "fingerprint": live["fingerprint"],
        "runtime_versions": live["runtime_versions"],
        "observed_candidate_commit": live.get("observed_candidate_commit"),
        "cases": cases,
    }


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Check live Python/C++ PP-OCR crop/Cls/Rec parity"
    )
    parser.add_argument("--check", action="store_true", help="fail on gate miss")
    parser.add_argument("--report-out", type=Path, default=None)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="sublift-paddle-rec-") as tmp:
        raw_root = Path(tmp)
        live = build_live_report(
            raw_dir=raw_root,
            report_with_timings=True,
        )
        model = live["fingerprint"]["model"]
        model_root = Path(str(model["root"])).expanduser()
        model_type = str(model["type"])
        frozen_candidates: dict[str, JsonDict] = {}
        for case in live["cases"]:
            case_id = str(case["case_id"])
            asset = FIXTURES_DIR / str(case["asset"])
            frozen_quads = _stage(
                case["oracle"], "4_det_postprocess"
            )["quads"]
            with Image.open(asset) as opened:
                frozen_candidates[case_id] = capture_cpp_trace(
                    opened.convert("RGB"),
                    repo_root=REPO_ROOT,
                    model_type=model_type,
                    model_root=model_root,
                    raw_dir=raw_root / case_id / "frozen_cpp",
                    frozen_quads=frozen_quads,
                )
        report = evaluate_rec_report(
            live,
            raw_root,
            frozen_candidates,
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
