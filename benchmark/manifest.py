"""Benchmark run manifest loader.

The manifest is a small JSON file that records the inputs needed to reproduce a
benchmark run, including the GUI-selected subtitle region when one is used.
Relative paths are resolved from the repository root discovered from the
manifest location.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from benchmark.runner import RunConfig
from sublift.diagnostics.performance import PerformanceMode, parse_performance_mode
from sublift.models import SCRIPT_AUTO, SCRIPT_VALUES


class ManifestError(ValueError):
    """Raised when a benchmark manifest is missing or malformed."""


def load_run_config(manifest_path: Path) -> RunConfig:
    """Load a benchmark manifest JSON file into ``RunConfig``.

    Supported keys:
        video / video_path: input video path.
        ground_truth / ground_truth_path: ground truth SRT path.
        fps: sampling FPS, default 5.0.
        engine: OCR engine, default "vision".
        confidence: OCR confidence threshold, default 0.5.
        subtitle_script: target script, default "auto".
        match_threshold: segment match threshold, default 0.5.
        region_box: optional [x, y, width, height] pixel box (source-frame).
        frame_output_mode: optional "full"|"roi" (default "full"); roi 需 region_box。
        label: optional report suffix.
        video_duration_seconds: optional duration override.
        output_dir: report output directory, default debug/benchmark-reports.
        performance: optional object::
            {
              "mode": "off"|"summary"|"trace",  # default off
              "warmup_runs": int >= 0,          # default 0
              "measured_runs": int >= 1         # default 1
            }
    """
    try:
        payload = json.loads(manifest_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise ManifestError(f"manifest 不是合法 JSON: {manifest_path}") from exc
    if not isinstance(payload, dict):
        raise ManifestError("manifest 顶层必须是 JSON object")

    root = _discover_repo_root(manifest_path)
    output_dir = _path(payload, ("output_dir",), root, required=False)
    perf_mode, warmup_runs, measured_runs = _performance(payload)
    region_box = _region_box(payload)
    frame_output_mode = _frame_output_mode(payload, region_box=region_box)
    return RunConfig(
        video_path=_required_path(payload, ("video", "video_path"), root),
        ground_truth_path=_required_path(payload, ("ground_truth", "ground_truth_path"), root),
        fps=_float(payload, "fps", default=5.0, exclusive_minimum=0.0),
        engine=_str(payload, "engine", default="vision"),
        confidence=_float(payload, "confidence", default=0.5, minimum=0.0, maximum=1.0),
        subtitle_script=_choice(
            payload,
            "subtitle_script",
            default=SCRIPT_AUTO,
            choices=SCRIPT_VALUES,
        ),
        match_threshold=_float(
            payload,
            "match_threshold",
            default=0.5,
            exclusive_minimum=0.0,
            maximum=1.0,
        ),
        region_box=region_box,
        label=_optional_str(payload, "label"),
        video_duration_seconds=_optional_float(
            payload, "video_duration_seconds", exclusive_minimum=0.0
        ),
        output_dir=output_dir or root / "debug/benchmark-reports",
        performance_mode=perf_mode,
        warmup_runs=warmup_runs,
        measured_runs=measured_runs,
        frame_output_mode=frame_output_mode,
    )


def _discover_repo_root(manifest_path: Path) -> Path:
    """Find the repo root by walking up from the manifest location."""
    current = manifest_path.resolve().parent
    for candidate in (current, *current.parents):
        has_project_file = (candidate / "pyproject.toml").exists()
        has_feature_list = (candidate / "feature-list.json").exists()
        if has_project_file and has_feature_list:
            return candidate
    return Path.cwd()


def _required_path(payload: dict[str, Any], keys: tuple[str, ...], root: Path) -> Path:
    path = _path(payload, keys, root, required=True)
    if path is None:
        joined = " / ".join(keys)
        raise ManifestError(f"manifest 缺少必填路径字段: {joined}")
    return path


def _path(
    payload: dict[str, Any],
    keys: tuple[str, ...],
    root: Path,
    *,
    required: bool,
) -> Path | None:
    value = _first_present(payload, keys)
    if value is None:
        if required:
            joined = " / ".join(keys)
            raise ManifestError(f"manifest 缺少必填路径字段: {joined}")
        return None
    if not isinstance(value, str) or not value:
        raise ManifestError(f"路径字段必须是非空字符串: {keys[0]}")
    path = Path(value)
    return path if path.is_absolute() else root / path


def _first_present(payload: dict[str, Any], keys: tuple[str, ...]) -> object | None:
    for key in keys:
        if key in payload:
            value: object = payload[key]
            return value
    return None


def _float(
    payload: dict[str, Any],
    key: str,
    *,
    default: float,
    minimum: float | None = None,
    exclusive_minimum: float | None = None,
    maximum: float | None = None,
) -> float:
    value = payload.get(key, default)
    if not isinstance(value, int | float):
        raise ManifestError(f"{key} 必须是数字")
    result = float(value)
    _validate_number_range(
        key,
        result,
        minimum=minimum,
        exclusive_minimum=exclusive_minimum,
        maximum=maximum,
    )
    return result


def _optional_float(
    payload: dict[str, Any],
    key: str,
    *,
    minimum: float | None = None,
    exclusive_minimum: float | None = None,
    maximum: float | None = None,
) -> float | None:
    if key not in payload or payload[key] is None:
        return None
    value = payload[key]
    if not isinstance(value, int | float):
        raise ManifestError(f"{key} 必须是数字")
    result = float(value)
    _validate_number_range(
        key,
        result,
        minimum=minimum,
        exclusive_minimum=exclusive_minimum,
        maximum=maximum,
    )
    return result


def _validate_number_range(
    key: str,
    value: float,
    *,
    minimum: float | None,
    exclusive_minimum: float | None,
    maximum: float | None,
) -> None:
    if minimum is not None and value < minimum:
        raise ManifestError(f"{key} 必须大于等于 {minimum}")
    if exclusive_minimum is not None and value <= exclusive_minimum:
        raise ManifestError(f"{key} 必须大于 {exclusive_minimum}")
    if maximum is not None and value > maximum:
        raise ManifestError(f"{key} 必须小于等于 {maximum}")


def _str(payload: dict[str, Any], key: str, *, default: str) -> str:
    value = payload.get(key, default)
    if not isinstance(value, str) or not value:
        raise ManifestError(f"{key} 必须是非空字符串")
    return value


def _optional_str(payload: dict[str, Any], key: str) -> str | None:
    value = payload.get(key)
    if value is None:
        return None
    if not isinstance(value, str) or not value:
        raise ManifestError(f"{key} 必须是非空字符串或 null")
    return value


def _choice(
    payload: dict[str, Any],
    key: str,
    *,
    default: str,
    choices: frozenset[str],
) -> str:
    value = _str(payload, key, default=default)
    if value not in choices:
        allowed = ", ".join(sorted(choices))
        raise ManifestError(f"{key} 必须是以下值之一: {allowed}")
    return value


def _region_box(payload: dict[str, Any]) -> tuple[int, int, int, int] | None:
    value = payload.get("region_box")
    if value is None:
        return None
    if not isinstance(value, list) or len(value) != 4:
        raise ManifestError("region_box 必须是 [x, y, width, height] 四元数组")
    if not all(isinstance(item, int) for item in value):
        raise ManifestError("region_box 的四个值必须都是整数")

    x, y, width, height = value
    if x < 0 or y < 0:
        raise ManifestError("region_box 的 x/y 不能为负")
    if width <= 0 or height <= 0:
        raise ManifestError("region_box 的 width/height 必须大于 0")
    return (x, y, width, height)


def _frame_output_mode(
    payload: dict[str, Any],
    *,
    region_box: tuple[int, int, int, int] | None,
) -> str:
    """解析 frame_output_mode；缺省 full；roi 需要 region_box。"""
    if "frame_output_mode" not in payload or payload["frame_output_mode"] is None:
        return "full"
    value = payload["frame_output_mode"]
    if not isinstance(value, str):
        raise ManifestError("frame_output_mode 必须是字符串")
    mode = value.strip().lower()
    if mode not in ("full", "roi"):
        raise ManifestError("frame_output_mode 必须是 'full' 或 'roi'")
    if mode == "roi" and region_box is None:
        raise ManifestError("frame_output_mode=roi 需要 region_box")
    return mode


def _performance(payload: dict[str, Any]) -> tuple[str, int, int]:
    """解析 performance 块；缺省为 off / 0 warmup / 1 measured。"""
    if "performance" not in payload or payload["performance"] is None:
        return PerformanceMode.OFF.value, 0, 1
    value = payload["performance"]
    if not isinstance(value, dict):
        raise ManifestError("performance 必须是 object")

    mode_raw = value.get("mode", PerformanceMode.OFF.value)
    if not isinstance(mode_raw, str):
        raise ManifestError("performance.mode 必须是字符串")
    try:
        mode = parse_performance_mode(mode_raw)
    except ValueError as exc:
        raise ManifestError(str(exc)) from exc

    warmup = value.get("warmup_runs", 0)
    measured = value.get("measured_runs", 1)
    if not isinstance(warmup, int) or isinstance(warmup, bool):
        raise ManifestError("performance.warmup_runs 必须是整数")
    if not isinstance(measured, int) or isinstance(measured, bool):
        raise ManifestError("performance.measured_runs 必须是整数")
    if warmup < 0:
        raise ManifestError("performance.warmup_runs 不能为负")
    if measured < 1:
        raise ManifestError("performance.measured_runs 必须 >= 1")
    return mode.value, warmup, measured
