"""Benchmark configuration loading and validation.

The loader accepts both the historical flat JSON manifest and the v2 document:

.. code-block:: json

   {
     "schema_version": 2,
     "run": {"video": "...", "ground_truth": "..."},
     "matrix": {"fps": [5, 8, 12], "engine": ["vision", "paddle"]}
   }

CLI overrides use dotted keys (for example ``performance.measured_runs=3``).
All paths remain relative to the repository root so a config can be invoked
from any working directory.
"""

from __future__ import annotations

import copy
import json
from collections.abc import Mapping
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any

from sublift.benchmark.pipeline_config import normalize_pipeline_overrides
from sublift.diagnostics.performance import PerformanceMode, parse_performance_mode
from sublift.models import SCRIPT_AUTO, SCRIPT_VALUES

_RUN_KEYS = frozenset(
    {
        "video",
        "video_path",
        "ground_truth",
        "ground_truth_path",
        "fps",
        "engine",
        "confidence",
        "subtitle_script",
        "match_threshold",
        "region_box",
        "frame_output_mode",
        "pipeline",
        "label",
        "video_duration_seconds",
        "output_dir",
        "performance",
        "isolate_processes",
    }
)
_PERFORMANCE_KEYS = frozenset({"mode", "warmup_runs", "measured_runs"})
_ENGINES = frozenset({"vision", "paddle", "mock"})


class ManifestError(ValueError):
    """Raised when a benchmark config is missing or malformed."""


@dataclass(frozen=True)
class RunConfig:
    """One resolved benchmark run.

    This model belongs to the configuration layer, not the runner. Keeping it
    here prevents config parsing from depending on execution internals.
    """

    video_path: Path
    ground_truth_path: Path
    fps: float = 5.0
    engine: str = "vision"
    confidence: float = 0.5
    subtitle_script: str = SCRIPT_AUTO
    match_threshold: float = 0.5
    region_box: tuple[int, int, int, int] | None = None
    label: str | None = None
    video_duration_seconds: float | None = None
    output_dir: Path = field(default_factory=lambda: Path("debug/benchmark/runs"))
    performance_mode: str = PerformanceMode.OFF.value
    warmup_runs: int = 0
    measured_runs: int = 1
    isolate_processes: bool = True
    frame_output_mode: str = "full"
    pipeline_overrides: dict[str, Any] = field(default_factory=dict)

    @property
    def output_prefix(self) -> str:
        """Return the stable report file prefix."""
        base = self.video_path.stem
        return f"{base}_{self.label}" if self.label else base


@dataclass(frozen=True)
class ManifestDocument:
    """Parsed benchmark config document before one run is resolved."""

    path: Path
    repo_root: Path
    run: dict[str, Any]
    matrix: dict[str, list[Any]]


def load_manifest(manifest_path: Path) -> ManifestDocument:
    """Load a v1 flat manifest or v2 run/matrix document."""
    try:
        raw = json.loads(manifest_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise ManifestError(f"benchmark config 不是合法 JSON: {manifest_path}") from exc
    if not isinstance(raw, dict):
        raise ManifestError("benchmark config 顶层必须是 JSON object")

    schema_version = raw.get("schema_version", 1)
    if schema_version not in (1, 2):
        raise ManifestError(f"不支持的 schema_version: {schema_version!r}")

    if "run" in raw:
        unknown_document_keys = set(raw) - {"schema_version", "run", "matrix"}
        if unknown_document_keys:
            keys = ", ".join(sorted(unknown_document_keys))
            raise ManifestError(f"v2 config 顶层包含未知字段: {keys}")
        run = raw["run"]
        if not isinstance(run, dict):
            raise ManifestError("run 必须是 object")
        run_payload = dict(run)
    else:
        run_payload = {
            key: value for key, value in raw.items() if key not in {"schema_version", "matrix"}
        }

    _validate_run_keys(run_payload)
    matrix = _matrix(raw.get("matrix"))
    root = discover_repo_root(manifest_path)
    return ManifestDocument(
        path=manifest_path.resolve(),
        repo_root=root,
        run=run_payload,
        matrix=matrix,
    )


def load_run_config(
    manifest_path: Path,
    *,
    overrides: Mapping[str, Any] | None = None,
) -> RunConfig:
    """Load and resolve one run, optionally applying dotted-key overrides."""
    return resolve_run_config(load_manifest(manifest_path), overrides=overrides)


def resolve_run_config(
    document: ManifestDocument,
    *,
    overrides: Mapping[str, Any] | None = None,
) -> RunConfig:
    """Resolve a :class:`ManifestDocument` into a validated run."""
    payload = copy.deepcopy(document.run)
    if overrides:
        apply_overrides(payload, overrides)
    _validate_run_keys(payload)

    output_dir = _path(payload, ("output_dir",), document.repo_root, required=False)
    perf_mode, warmup_runs, measured_runs = _performance(payload)
    region_box = _region_box(payload)
    frame_output_mode = _frame_output_mode(payload, region_box=region_box)
    engine = _choice(payload, "engine", default="vision", choices=_ENGINES)

    return RunConfig(
        video_path=_required_path(payload, ("video", "video_path"), document.repo_root),
        ground_truth_path=_required_path(
            payload,
            ("ground_truth", "ground_truth_path"),
            document.repo_root,
        ),
        fps=_float(payload, "fps", default=5.0, exclusive_minimum=0.0),
        engine=engine,
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
            payload,
            "video_duration_seconds",
            exclusive_minimum=0.0,
        ),
        output_dir=output_dir or document.repo_root / "debug/benchmark/runs",
        performance_mode=perf_mode,
        warmup_runs=warmup_runs,
        measured_runs=measured_runs,
        isolate_processes=_bool(payload, "isolate_processes", default=True),
        frame_output_mode=frame_output_mode,
        pipeline_overrides=_pipeline(payload),
    )


def apply_overrides(payload: dict[str, Any], overrides: Mapping[str, Any]) -> None:
    """Apply validated dotted-key overrides to a mutable run payload."""
    for raw_key, value in overrides.items():
        key = raw_key.strip()
        if not key:
            raise ManifestError("override key 不能为空")
        parts = key.split(".")
        if any(not part for part in parts):
            raise ManifestError(f"非法 override key: {raw_key!r}")
        target = payload
        for part in parts[:-1]:
            current = target.get(part)
            if current is None:
                nested: dict[str, Any] = {}
                target[part] = nested
                target = nested
            elif isinstance(current, dict):
                target = current
            else:
                raise ManifestError(f"override 不能穿透非 object 字段: {raw_key}")
        target[parts[-1]] = value


def config_to_dict(config: RunConfig) -> dict[str, Any]:
    """Serialize a resolved config for ``show`` and matrix plans."""
    payload = asdict(config)
    payload["pipeline"] = payload.pop("pipeline_overrides")
    payload["video_path"] = str(config.video_path)
    payload["ground_truth_path"] = str(config.ground_truth_path)
    payload["output_dir"] = str(config.output_dir)
    payload["region_box"] = list(config.region_box) if config.region_box else None
    return payload


def discover_repo_root(manifest_path: Path) -> Path:
    """Find the repository root by walking upward from the config."""
    current = manifest_path.resolve().parent
    for candidate in (current, *current.parents):
        if (candidate / "pyproject.toml").exists() and (candidate / "feature-list.json").exists():
            return candidate
    return Path.cwd()


def _validate_run_keys(payload: dict[str, Any]) -> None:
    unknown = set(payload) - _RUN_KEYS
    if unknown:
        keys = ", ".join(sorted(unknown))
        raise ManifestError(f"run 包含未知字段: {keys}")
    performance = payload.get("performance")
    if isinstance(performance, dict):
        unknown_performance = set(performance) - _PERFORMANCE_KEYS
        if unknown_performance:
            keys = ", ".join(sorted(unknown_performance))
            raise ManifestError(f"performance 包含未知字段: {keys}")


def _matrix(value: object) -> dict[str, list[Any]]:
    if value is None:
        return {}
    if not isinstance(value, dict):
        raise ManifestError("matrix 必须是 object")
    axes: dict[str, list[Any]] = {}
    for key, candidates in value.items():
        if not isinstance(key, str) or not key:
            raise ManifestError("matrix 参数名必须是非空字符串")
        if not isinstance(candidates, list) or not candidates:
            raise ManifestError(f"matrix.{key} 必须是非空数组")
        axes[key] = list(candidates)
    return axes


def _required_path(payload: dict[str, Any], keys: tuple[str, ...], root: Path) -> Path:
    path = _path(payload, keys, root, required=True)
    if path is None:
        joined = " / ".join(keys)
        raise ManifestError(f"config 缺少必填路径字段: {joined}")
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
            raise ManifestError(f"config 缺少必填路径字段: {joined}")
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
    if isinstance(value, bool) or not isinstance(value, int | float):
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
    if isinstance(value, bool) or not isinstance(value, int | float):
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


def _bool(payload: dict[str, Any], key: str, *, default: bool) -> bool:
    value = payload.get(key, default)
    if not isinstance(value, bool):
        raise ManifestError(f"{key} 必须是 boolean")
    return value


def _region_box(payload: dict[str, Any]) -> tuple[int, int, int, int] | None:
    value = payload.get("region_box")
    if value is None:
        return None
    if not isinstance(value, list) or len(value) != 4:
        raise ManifestError("region_box 必须是 [x, y, width, height] 四元数组")
    if not all(isinstance(item, int) and not isinstance(item, bool) for item in value):
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
    value = payload.get("performance")
    if value is None:
        return PerformanceMode.OFF.value, 0, 1
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


def _pipeline(payload: dict[str, Any]) -> dict[str, Any]:
    try:
        return normalize_pipeline_overrides(payload.get("pipeline"))
    except ValueError as exc:
        raise ManifestError(str(exc)) from exc
