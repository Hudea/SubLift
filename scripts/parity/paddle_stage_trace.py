"""Live Paddle stage tracing shared by the Phase 6.8 dump and parity tools.

The Python path records the real RapidOCR call graph by wrapping its detector,
classifier, recognizer, and perspective-crop callables. The C++ path invokes
the diagnostic-only ``sublift_paddle_trace`` executable with the same RGB24
fixture. Normal product OCR remains unchanged and does not retain tensors.
"""

from __future__ import annotations

import copy
import hashlib
import json
import os
import subprocess
import tempfile
import time
from collections.abc import Callable
from pathlib import Path
from typing import Any, cast

import numpy as np
from PIL import Image

from sublift.ocr.paddle import PaddleOcrEngine

JsonDict = dict[str, Any]


def sha256_prefixed(data: bytes) -> str:
    return "sha256:" + hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    return sha256_prefixed(path.read_bytes())


def _canonical_array(array: np.ndarray) -> tuple[np.ndarray, str]:
    """Return a contiguous, little-endian array with a stable dtype label."""
    value = np.asarray(array)
    if np.issubdtype(value.dtype, np.floating):
        canonical = np.ascontiguousarray(value, dtype="<f4")
        return canonical, "float32-le"
    if value.dtype == np.uint8:
        return np.ascontiguousarray(value, dtype=np.uint8), "uint8"
    if np.issubdtype(value.dtype, np.integer):
        canonical = np.ascontiguousarray(value, dtype="<i4")
        return canonical, "int32-le"
    raise TypeError(f"unsupported stage array dtype: {value.dtype}")


def array_summary(
    array: np.ndarray,
    *,
    raw_dir: Path | None = None,
    raw_name: str | None = None,
    color_order: str | None = None,
) -> JsonDict:
    canonical, dtype_name = _canonical_array(array)
    raw = canonical.tobytes(order="C")
    flat = canonical.reshape(-1)
    result: JsonDict = {
        "dtype": dtype_name,
        "shape": list(canonical.shape),
        "count": int(canonical.size),
        "sha256": sha256_prefixed(raw),
        "min": float(np.min(flat)) if flat.size else None,
        "max": float(np.max(flat)) if flat.size else None,
        "mean": float(np.mean(flat, dtype=np.float64)) if flat.size else None,
    }
    if color_order is not None:
        result["color_order"] = color_order
    if raw_dir is not None and raw_name is not None:
        raw_dir.mkdir(parents=True, exist_ok=True)
        filename = f"{raw_name}.bin"
        (raw_dir / filename).write_bytes(raw)
        result["raw_file"] = filename
    return result


def _clone_stage_value(value: Any) -> Any:
    if isinstance(value, np.ndarray):
        return value.copy()
    if isinstance(value, tuple):
        return tuple(_clone_stage_value(item) for item in value)
    if isinstance(value, list):
        return [_clone_stage_value(item) for item in value]
    return copy.deepcopy(value)


class _RecordingCallable:
    """Transparent callable proxy that records inputs, outputs, and wall time."""

    def __init__(self, delegate: Any) -> None:
        object.__setattr__(self, "_delegate", delegate)
        object.__setattr__(self, "calls", [])
        object.__setattr__(self, "elapsed_ms", 0.0)

    def __call__(self, *args: Any, **kwargs: Any) -> Any:
        start = time.perf_counter()
        result = self._delegate(*args, **kwargs)
        self.elapsed_ms += (time.perf_counter() - start) * 1000.0
        self.calls.append(
            {
                "args": _clone_stage_value(args),
                "kwargs": _clone_stage_value(kwargs),
                "result": _clone_stage_value(result),
            }
        )
        return result

    def __getattr__(self, name: str) -> Any:
        return getattr(self._delegate, name)

    def __setattr__(self, name: str, value: Any) -> None:
        if name in {"_delegate", "calls", "elapsed_ms"}:
            object.__setattr__(self, name, value)
            return
        setattr(self._delegate, name, value)


def _tensor_calls(
    recorder: _RecordingCallable,
    *,
    field: str,
    raw_dir: Path | None,
    prefix: str,
) -> list[JsonDict]:
    summaries: list[JsonDict] = []
    for index, call in enumerate(recorder.calls):
        value = call[field]
        if field == "args":
            value = value[0]
        summaries.append(
            array_summary(
                np.asarray(value),
                raw_dir=raw_dir,
                raw_name=f"{prefix}_{index}",
            )
        )
    return summaries


def _quad_list(boxes: Any, scores: Any) -> list[JsonDict]:
    if boxes is None:
        return []
    box_array = np.asarray(boxes)
    score_values = list(scores or [])
    results: list[JsonDict] = []
    for index, box in enumerate(box_array):
        points = np.asarray(box, dtype=np.float32).reshape(-1, 2)
        results.append(
            {
                "points": [[float(x), float(y)] for x, y in points],
                "score": (
                    float(score_values[index])
                    if index < len(score_values)
                    else None
                ),
            }
        )
    return results


def _line_json(line: Any) -> JsonDict:
    return {
        "text": line.text,
        "confidence": float(line.confidence),
        "box": {
            "x": int(line.box.x),
            "y": int(line.box.y),
            "width": int(line.box.width),
            "height": int(line.box.height),
        },
    }


def _ctc_tokens(logits: np.ndarray) -> list[list[int]]:
    """Return collapsed non-blank argmax tokens for each Rec batch item."""
    value = np.asarray(logits)
    if value.ndim != 3:
        return []
    indices = np.argmax(value, axis=2)
    all_tokens: list[list[int]] = []
    for row in indices:
        tokens: list[int] = []
        previous = 0
        for token_value in row:
            token = int(token_value)
            if token != 0 and token != previous:
                tokens.append(token)
            previous = token
        all_tokens.append(tokens)
    return all_tokens


def capture_python_trace(
    image: Image.Image,
    *,
    model_type: str = "small",
    model_root: Path | None = None,
    raw_dir: Path | None = None,
) -> JsonDict:
    """Capture one real RapidOCR recognize call without changing its output."""
    wrapper = PaddleOcrEngine(
        model_type=model_type,
        model_root_dir=model_root,
    )
    rapid = wrapper._engine

    source_rgb = np.asarray(image.convert("RGB"), dtype=np.uint8)
    loaded_bgr = rapid.load_img(image)
    global_preprocessed, op_record = rapid.preprocess_img(loaded_bgr)
    if rapid.cfg.Global.use_vertical_padding:
        from rapidocr.utils.process_img import apply_vertical_padding

        working_bgr, op_record = apply_vertical_padding(
            global_preprocessed,
            op_record,
            rapid.width_height_ratio,
            rapid.min_height,
        )
    else:
        working_bgr = global_preprocessed
        op_record["padding_1"] = {"top": 0, "left": 0}

    det_session = _RecordingCallable(rapid.text_det.session)
    det_postprocess = _RecordingCallable(rapid.text_det.postprocess_op)
    cls_session = _RecordingCallable(rapid.text_cls.session)
    cls_postprocess = _RecordingCallable(rapid.text_cls.postprocess_op)
    rec_session = _RecordingCallable(rapid.text_rec.session)
    rec_postprocess = _RecordingCallable(rapid.text_rec.postprocess_op)

    text_det = cast(Any, rapid.text_det)
    text_cls = cast(Any, rapid.text_cls)
    text_rec = cast(Any, rapid.text_rec)
    text_det.session = det_session
    text_det.postprocess_op = det_postprocess
    text_cls.session = cls_session
    text_cls.postprocess_op = cls_postprocess
    text_rec.session = rec_session
    text_rec.postprocess_op = rec_postprocess

    import rapidocr.main as rapid_main

    rapid_main_module = cast(Any, rapid_main)
    original_crop = cast(
        Callable[[np.ndarray, np.ndarray], np.ndarray],
        rapid_main_module.get_rotate_crop_image,
    )
    crop_images: list[tuple[np.ndarray, bool]] = []
    crop_elapsed_ms = 0.0

    def record_crop(img: np.ndarray, points: np.ndarray) -> np.ndarray:
        nonlocal crop_elapsed_ms
        point_array = np.asarray(points, dtype=np.float32)
        crop_width = max(
            float(np.linalg.norm(point_array[0] - point_array[1])),
            float(np.linalg.norm(point_array[2] - point_array[3])),
        )
        crop_height = max(
            float(np.linalg.norm(point_array[0] - point_array[3])),
            float(np.linalg.norm(point_array[1] - point_array[2])),
        )
        rotated_90 = crop_width > 0.0 and crop_height / crop_width >= 1.5
        start = time.perf_counter()
        crop = original_crop(img, points)
        crop_elapsed_ms += (time.perf_counter() - start) * 1000.0
        crop_images.append((crop.copy(), rotated_90))
        return crop

    rapid_main_module.get_rotate_crop_image = record_crop
    total_start = time.perf_counter()
    try:
        result = wrapper.recognize(image)
    finally:
        rapid_main_module.get_rotate_crop_image = original_crop
        text_det.session = det_session._delegate
        text_det.postprocess_op = det_postprocess._delegate
        text_cls.session = cls_session._delegate
        text_cls.postprocess_op = cls_postprocess._delegate
        text_rec.session = rec_session._delegate
        text_rec.postprocess_op = rec_postprocess._delegate
    total_ms = (time.perf_counter() - total_start) * 1000.0

    det_inputs = _tensor_calls(
        det_session,
        field="args",
        raw_dir=raw_dir,
        prefix="det_input",
    )
    det_outputs = _tensor_calls(
        det_session,
        field="result",
        raw_dir=raw_dir,
        prefix="det_probability",
    )
    cls_inputs = _tensor_calls(
        cls_session,
        field="args",
        raw_dir=raw_dir,
        prefix="cls_input",
    )
    cls_outputs = _tensor_calls(
        cls_session,
        field="result",
        raw_dir=raw_dir,
        prefix="cls_logits",
    )
    rec_inputs = _tensor_calls(
        rec_session,
        field="args",
        raw_dir=raw_dir,
        prefix="rec_input",
    )
    rec_outputs = _tensor_calls(
        rec_session,
        field="result",
        raw_dir=raw_dir,
        prefix="rec_logits",
    )

    det_quads: list[JsonDict] = []
    if det_postprocess.calls:
        boxes, scores = det_postprocess.calls[-1]["result"]
        if boxes is not None:
            boxes = rapid.text_det.sorted_boxes(np.asarray(boxes))
        det_quads = _quad_list(boxes, scores)

    cls_results: list[JsonDict] = []
    for call in cls_postprocess.calls:
        for label, score in call["result"]:
            cls_results.append({"label": str(label), "score": float(score)})

    decoded_results: list[JsonDict] = []
    rec_call_tokens = [
        batch_tokens
        for call in rec_session.calls
        for batch_tokens in _ctc_tokens(np.asarray(call["result"]))
    ]
    rec_result_index = 0
    for call in rec_postprocess.calls:
        line_results, _word_results = call["result"]
        for text, score in line_results:
            tokens = (
                rec_call_tokens[rec_result_index]
                if rec_result_index < len(rec_call_tokens)
                else []
            )
            decoded_results.append(
                {
                    "tokens": tokens,
                    "text": str(text),
                    "confidence": float(score),
                }
            )
            rec_result_index += 1

    crop_summaries = [
        {
            "image": array_summary(
                crop,
                raw_dir=raw_dir,
                raw_name=f"crop_{index}_bgr",
                color_order="BGR",
            ),
            "rotated_90": rotated_90,
        }
        for index, (crop, rotated_90) in enumerate(crop_images)
    ]

    output_lines = [_line_json(line) for line in result.lines]
    det_box_count = len(det_quads)
    return {
        "schema_version": 1,
        "runtime": "python",
        "stages": {
            "1_global_preprocess": {
                "source_image": array_summary(
                    source_rgb,
                    raw_dir=raw_dir,
                    raw_name="input_rgb",
                    color_order="RGB",
                ),
                "loaded_image": array_summary(
                    loaded_bgr,
                    raw_dir=raw_dir,
                    raw_name="loaded_bgr",
                    color_order="BGR",
                ),
                "working_image": array_summary(
                    working_bgr,
                    raw_dir=raw_dir,
                    raw_name="working_bgr",
                    color_order="BGR",
                ),
                "operations": copy.deepcopy(op_record),
            },
            "2_det_preprocess": {
                "tensor": det_inputs[0] if det_inputs else None,
            },
            "3_det_infer": {
                "probability_map": det_outputs[0] if det_outputs else None,
            },
            "4_det_postprocess": {
                "box_count": det_box_count,
                "quads": det_quads,
            },
            "5_perspective_crop": {
                "crop_count": len(crop_summaries),
                "crops": crop_summaries,
            },
            "6_cls": {
                "enabled": bool(rapid.use_cls),
                "batch_count": len(cls_inputs),
                "tensors": cls_inputs,
                "logits": cls_outputs,
                "results": cls_results,
            },
            "7_rec_preprocess": {
                "batch_count": len(rec_inputs),
                "tensors": rec_inputs,
            },
            "8_rec_infer": {
                "batch_count": len(rec_outputs),
                "logits": rec_outputs,
            },
            "9_rec_decode": {
                "decoded_count": len(decoded_results),
                "results": decoded_results,
            },
            "10_output": {
                "text": result.text,
                "confidence": float(result.confidence),
                "line_count": len(output_lines),
                "lines": output_lines,
            },
        },
        "counts": {
            "recognize_calls": 1,
            "det_calls": len(det_session.calls),
            "det_boxes": det_box_count,
            "cls_calls": len(cls_session.calls),
            "rec_calls": len(rec_session.calls),
        },
        "timing_ms": {
            "det_infer": det_session.elapsed_ms,
            "det_postprocess": det_postprocess.elapsed_ms,
            "crop": crop_elapsed_ms,
            "cls_infer": cls_session.elapsed_ms,
            "cls_postprocess": cls_postprocess.elapsed_ms,
            "rec_infer": rec_session.elapsed_ms,
            "rec_decode": rec_postprocess.elapsed_ms,
            "total": total_ms,
        },
    }


def resolve_cpp_trace_bin(repo_root: Path) -> Path | None:
    env_path = os.environ.get("SUBLIFT_PADDLE_TRACE_BIN")
    if env_path:
        candidate = Path(env_path)
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    for relative in (
        Path("build/cpp-rel/bin/sublift_paddle_trace"),
        Path("build/cpp/bin/sublift_paddle_trace"),
    ):
        candidate = repo_root / relative
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    return None


def capture_cpp_trace(
    image: Image.Image,
    *,
    repo_root: Path,
    model_type: str = "small",
    model_root: Path | None = None,
    raw_dir: Path | None = None,
    frozen_quads: list[JsonDict] | None = None,
    intra_op_threads: int | None = None,
    cls_batch_size: int | None = None,
    rec_batch_size: int | None = None,
) -> JsonDict:
    trace_bin = resolve_cpp_trace_bin(repo_root)
    if trace_bin is None:
        raise RuntimeError(
            "sublift_paddle_trace is unavailable; build C++ with "
            "SUBLIFT_ENABLE_PADDLE=ON"
        )

    rgb = np.ascontiguousarray(np.asarray(image.convert("RGB")), dtype=np.uint8)
    with tempfile.TemporaryDirectory(prefix="sublift-paddle-trace-") as tmp:
        tmp_dir = Path(tmp)
        rgb_path = tmp_dir / "input.rgb"
        output_path = tmp_dir / "trace.json"
        rgb_path.write_bytes(rgb.tobytes(order="C"))
        command = [
            str(trace_bin),
            "--rgb",
            str(rgb_path),
            "--width",
            str(image.width),
            "--height",
            str(image.height),
            "--out",
            str(output_path),
            "--model-type",
            model_type,
        ]
        if model_root is not None:
            command.extend(["--model-root", str(model_root)])
        if raw_dir is not None:
            command.extend(["--raw-dir", str(raw_dir)])
        if frozen_quads is not None:
            quads_path = tmp_dir / "frozen-quads.json"
            quads_path.write_text(
                json.dumps(frozen_quads, ensure_ascii=False),
                encoding="utf-8",
            )
            command.extend(["--quads-json", str(quads_path)])
        if intra_op_threads is not None:
            command.extend(["--intra-op-threads", str(intra_op_threads)])
        if cls_batch_size is not None:
            command.extend(["--cls-batch-size", str(cls_batch_size)])
        if rec_batch_size is not None:
            command.extend(["--rec-batch-size", str(rec_batch_size)])
        completed = subprocess.run(
            command,
            capture_output=True,
            text=True,
            timeout=120,
            check=False,
        )
        if completed.returncode != 0:
            detail = (completed.stderr or completed.stdout).strip()
            raise RuntimeError(f"C++ Paddle trace failed: {detail}")
        return cast(
            JsonDict,
            json.loads(output_path.read_text(encoding="utf-8")),
        )


def normalize_trace_for_golden(trace: JsonDict) -> JsonDict:
    """Drop observational timing and machine-local raw paths."""
    normalized = copy.deepcopy(trace)
    normalized.pop("timing_ms", None)
    normalized.pop("observed_candidate_commit", None)

    def walk(value: Any) -> None:
        if isinstance(value, dict):
            value.pop("timing_ms", None)
            value.pop("observed_candidate_commit", None)
            value.pop("raw_file", None)
            for child in value.values():
                walk(child)
        elif isinstance(value, list):
            for child in value:
                walk(child)

    walk(normalized)
    return normalized
