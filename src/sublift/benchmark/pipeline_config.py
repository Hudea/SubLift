"""Validated benchmark overrides for :mod:`sublift.config`.

Benchmark configs deliberately expose pipeline tuning under a dedicated
``pipeline`` object.  The allow-list is derived from the pipeline dataclasses,
so adding a new scalar field to ``Config`` automatically makes it available to
future benchmark matrices without widening the top-level benchmark schema.
"""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import fields, replace
from typing import Any

from sublift.config import ChangePointConfig, Config, SignatureConfig

_RESERVED_FIELDS = frozenset(
    {
        "sample_fps",
        "confidence_threshold",
        "subtitle_profile",
        "subtitle_script",
    }
)
_NESTED_DEFAULTS: dict[str, SignatureConfig | ChangePointConfig] = {
    "signature": SignatureConfig(),
    "change_point": ChangePointConfig(),
}
_CONFIG_DEFAULT = Config()
_SCALAR_DEFAULTS = {
    item.name: getattr(_CONFIG_DEFAULT, item.name)
    for item in fields(Config)
    if item.name not in _RESERVED_FIELDS and item.name not in _NESTED_DEFAULTS
}
_ZERO_TO_ONE_PATHS = frozenset(
    {
        "pipeline.low_conf_threshold",
        "pipeline.line_select_min_score",
        "pipeline.line_select_min_script",
        "pipeline.change_point.presence_threshold",
        "pipeline.change_point.ssim_threshold",
        "pipeline.change_point.ssim_patrol_threshold",
    }
)
_POSITIVE_PATHS = frozenset(
    {
        "pipeline.region_bottom_ratio",
        "pipeline.ocr_consensus_frames",
        "pipeline.signature.block_size_ratio",
        "pipeline.signature.hash_size",
        "pipeline.change_point.hysteresis_frames",
        "pipeline.change_point.ssim_window_size",
        "pipeline.change_point.ssim_patrol_interval",
    }
)
_NON_NEGATIVE_PATHS = frozenset(
    {
        "pipeline.merge_gap_ms",
        "pipeline.min_duration_ms",
        "pipeline.ocr_anchor_delay_frames",
        "pipeline.change_point.change_threshold",
    }
)


def normalize_pipeline_overrides(value: object) -> dict[str, Any]:
    """Validate and normalize a benchmark ``pipeline`` object."""
    if value is None:
        return {}
    if not isinstance(value, dict):
        raise ValueError("pipeline 必须是 object")

    unknown = set(value) - set(_SCALAR_DEFAULTS) - set(_NESTED_DEFAULTS)
    if unknown:
        keys = ", ".join(sorted(unknown))
        raise ValueError(f"pipeline 包含未知或保留字段: {keys}")

    normalized: dict[str, Any] = {}
    for key, raw_value in value.items():
        if key in _NESTED_DEFAULTS:
            normalized[key] = _normalize_nested(
                key,
                raw_value,
                _NESTED_DEFAULTS[key],
            )
        else:
            normalized[key] = _normalize_scalar(
                f"pipeline.{key}",
                raw_value,
                _SCALAR_DEFAULTS[key],
            )
    return normalized


def apply_pipeline_overrides(base: Config, overrides: Mapping[str, Any]) -> Config:
    """Return ``base`` with validated benchmark pipeline overrides applied."""
    normalized = normalize_pipeline_overrides(dict(overrides))
    scalar_changes = {
        key: value for key, value in normalized.items() if key not in _NESTED_DEFAULTS
    }
    if "signature" in normalized:
        scalar_changes["signature"] = replace(base.signature, **normalized["signature"])
    if "change_point" in normalized:
        scalar_changes["change_point"] = replace(
            base.change_point,
            **normalized["change_point"],
        )
    return replace(base, **scalar_changes)


def _normalize_nested(
    name: str,
    value: object,
    default: SignatureConfig | ChangePointConfig,
) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ValueError(f"pipeline.{name} 必须是 object")
    defaults = {item.name: getattr(default, item.name) for item in fields(default)}
    unknown = set(value) - set(defaults)
    if unknown:
        keys = ", ".join(sorted(unknown))
        raise ValueError(f"pipeline.{name} 包含未知字段: {keys}")
    return {
        key: _normalize_scalar(f"pipeline.{name}.{key}", raw_value, defaults[key])
        for key, raw_value in value.items()
    }


def _normalize_scalar(path: str, value: object, default: object) -> object:
    normalized: object
    if isinstance(default, bool):
        if not isinstance(value, bool):
            raise ValueError(f"{path} 必须是 boolean")
        normalized = value
    elif isinstance(default, int):
        if not isinstance(value, int) or isinstance(value, bool):
            raise ValueError(f"{path} 必须是整数")
        normalized = value
    elif isinstance(default, float):
        if not isinstance(value, int | float) or isinstance(value, bool):
            raise ValueError(f"{path} 必须是数字")
        normalized = float(value)
    elif isinstance(default, str):
        if not isinstance(value, str) or not value:
            raise ValueError(f"{path} 必须是非空字符串")
        normalized = value
    else:
        raise ValueError(f"{path} 的类型暂不支持 benchmark 覆盖")
    _validate_constraint(path, normalized)
    return normalized


def _validate_constraint(path: str, value: object) -> None:
    if not isinstance(value, int | float) or isinstance(value, bool):
        return
    numeric = float(value)
    if path in _ZERO_TO_ONE_PATHS and not 0.0 <= numeric <= 1.0:
        raise ValueError(f"{path} 必须在 0 到 1 之间")
    if path in _POSITIVE_PATHS and numeric <= 0.0:
        raise ValueError(f"{path} 必须大于 0")
    if path in _NON_NEGATIVE_PATHS and numeric < 0.0:
        raise ValueError(f"{path} 不能为负")
    if path in {
        "pipeline.region_bottom_ratio",
        "pipeline.signature.block_size_ratio",
    } and numeric > 1.0:
        raise ValueError(f"{path} 必须小于等于 1")
    if path == "pipeline.change_point.ssim_window_size" and int(numeric) % 2 == 0:
        raise ValueError(f"{path} 必须是奇数")
