"""Generic override parsing and Cartesian benchmark matrix expansion."""

from __future__ import annotations

import itertools
import json
import re
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from typing import Any

from sublift.benchmark.config import ManifestDocument, ManifestError, RunConfig, resolve_run_config


@dataclass(frozen=True)
class MatrixCell:
    """One resolved point in a parameter matrix."""

    index: int
    label: str
    overrides: dict[str, Any]
    config: RunConfig


def parse_set_args(assignments: Sequence[str]) -> dict[str, Any]:
    """Parse repeated ``KEY=VALUE`` assignments.

    Values use JSON when possible, with an unquoted string fallback.
    """
    parsed: dict[str, Any] = {}
    for assignment in assignments:
        key, raw_value = _split_assignment(assignment)
        parsed[key] = parse_value(raw_value)
    return parsed


def parse_vary_args(assignments: Sequence[str]) -> dict[str, list[Any]]:
    """Parse repeated matrix axes.

    Preferred syntax is ``fps=[5,8,12]``. For simple scalar values,
    ``engine=vision,paddle`` is also accepted.
    """
    parsed: dict[str, list[Any]] = {}
    for assignment in assignments:
        key, raw_value = _split_assignment(assignment)
        value = parse_value(raw_value)
        if isinstance(value, list):
            candidates = value
        else:
            candidates = [parse_value(part.strip()) for part in raw_value.split(",")]
        if not candidates:
            raise ManifestError(f"matrix axis {key!r} 不能为空")
        parsed[key] = candidates
    return parsed


def parse_value(raw_value: str) -> Any:
    """Parse one CLI value as JSON, falling back to a plain string."""
    try:
        return json.loads(raw_value)
    except json.JSONDecodeError:
        return raw_value


def expand_matrix(
    document: ManifestDocument,
    *,
    fixed_overrides: Mapping[str, Any] | None = None,
    cli_axes: Mapping[str, list[Any]] | None = None,
    base_label: str,
    max_cases: int = 64,
) -> list[MatrixCell]:
    """Resolve the Cartesian product of document and CLI matrix axes."""
    if max_cases < 1:
        raise ManifestError("max_cases 必须 >= 1")
    axes = {key: list(values) for key, values in document.matrix.items()}
    if cli_axes:
        axes.update({key: list(values) for key, values in cli_axes.items()})
    if not axes:
        raise ManifestError("matrix 未定义参数；请在 config.matrix 或 --vary 中提供")

    forbidden = {"label", "output_dir"}
    invalid = forbidden.intersection(axes)
    if invalid:
        keys = ", ".join(sorted(invalid))
        raise ManifestError(f"matrix 不允许改变输出身份字段: {keys}")

    names = list(axes)
    values = [axes[name] for name in names]
    case_count = 1
    for candidates in values:
        case_count *= len(candidates)
    if case_count > max_cases:
        raise ManifestError(
            f"matrix 将生成 {case_count} 组，超过 --max-cases={max_cases}；"
            "请缩小范围或显式提高上限"
        )

    fixed = dict(fixed_overrides or {})
    cells: list[MatrixCell] = []
    for index, combination in enumerate(itertools.product(*values), start=1):
        varied = dict(zip(names, combination, strict=True))
        overrides = {**fixed, **varied}
        config = resolve_run_config(document, overrides=overrides)
        suffix = "__".join(f"{_slug(key)}-{_slug_value(value)}" for key, value in varied.items())
        label = f"{_slug(base_label)}__{suffix}"
        cells.append(
            MatrixCell(
                index=index,
                label=label,
                overrides=overrides,
                config=config,
            )
        )
    return cells


def _split_assignment(assignment: str) -> tuple[str, str]:
    if "=" not in assignment:
        raise ManifestError(f"参数覆盖必须是 KEY=VALUE: {assignment!r}")
    key, raw_value = assignment.split("=", 1)
    key = key.strip()
    raw_value = raw_value.strip()
    if not key or not raw_value:
        raise ManifestError(f"参数覆盖必须是 KEY=VALUE: {assignment!r}")
    return key, raw_value


def _slug(value: str) -> str:
    cleaned = re.sub(r"[^0-9A-Za-z._-]+", "-", value).strip("-")
    return cleaned or "value"


def _slug_value(value: Any) -> str:
    if isinstance(value, list):
        return "x".join(_slug_value(item) for item in value)
    if value is None:
        return "null"
    if isinstance(value, bool):
        return "true" if value else "false"
    return _slug(str(value))
