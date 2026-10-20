#!/usr/bin/env python3
"""Validate SubLift contract fixtures against schemas and reference semantics."""
from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
CONTRACTS = ROOT / "contracts"
FIXTURES = CONTRACTS / "fixtures"
SCHEMAS = CONTRACTS / "schemas"

JOB_STATUSES = [
    "queued",
    "preparing",
    "extracting",
    "finalizing",
    "completed",
    "failed",
    "cancelled",
    "interrupted",
]
TASK_STATUSES = [
    "waiting",
    "preparing",
    "extracting",
    "exporting",
    "completed",
    "failed",
    "cancelled",
    "interrupted",
    "skipped",
]


def load(path: Path) -> Any:
    with path.open("r", encoding="utf-8") as fh:
        return json.load(fh)


def srt_timestamp(ms: int) -> str:
    total = max(0, int(ms))
    hours = total // 3_600_000
    minutes = (total % 3_600_000) // 60_000
    seconds = (total % 60_000) // 1000
    millis = total % 1000
    return f"{hours:02d}:{minutes:02d}:{seconds:02d},{millis:03d}"


def format_srt(entries: list[dict[str, Any]]) -> str:
    ordered = sorted(entries, key=lambda item: (int(item["start_ms"]), int(item["end_ms"])))
    blocks: list[str] = []
    for entry in ordered:
        text = str(entry.get("text", ""))
        if not text.strip():
            continue
        start = max(0, int(entry["start_ms"]))
        end = max(start, int(entry["end_ms"]))
        blocks.append(f"{len(blocks) + 1}\n{srt_timestamp(start)} --> {srt_timestamp(end)}\n{text}")
    return "\n\n".join(blocks) + "\n\n" if blocks else ""


def round_half_up(value: float) -> int:
    return int(value + 0.5)


def convert_roi(normalized: dict[str, float], width: int, height: int) -> dict[str, int]:
    x = round_half_up(float(normalized["x"]) * width)
    y = round_half_up(float(normalized["y"]) * height)
    w = round_half_up(float(normalized["width"]) * width)
    h = round_half_up(float(normalized["height"]) * height)
    x = min(max(x, 0), width - 1)
    y = min(max(y, 0), height - 1)
    w = min(max(w, 1), width - x)
    h = min(max(h, 1), height - y)
    return {"x": x, "y": y, "width": w, "height": h}


def plan_output(
    target_name: str, existing_names: list[str], policy: str
) -> tuple[str, str | None]:
    existing = set(existing_names)
    if target_name not in existing:
        return "saved", target_name
    if policy == "skip":
        return "skipped", None
    if policy == "replace":
        return "saved", target_name
    if policy != "deterministic_rename":
        raise ValueError(f"unknown output policy: {policy}")
    path = Path(target_name)
    stem, suffix = path.stem, path.suffix
    index = 1
    while f"{stem}_{index}{suffix}" in existing:
        index += 1
    return "saved", f"{stem}_{index}{suffix}"


def check_defaults(errors: list[str]) -> None:
    data = load(FIXTURES / "defaults.json")
    canonical = data.get("canonical", {})
    expected_fps = {"fast": 5, "balanced": 8, "fine": 12}
    if canonical.get("sampling_fps") != expected_fps:
        errors.append(f"defaults: sampling_fps must be {expected_fps}")
    if canonical.get("confidence") != 0.0:
        errors.append("defaults: confidence must be 0.0")
    if canonical.get("script") != "auto":
        errors.append("defaults: script must be auto")
    if canonical.get("roi") != {"policy": "auto", "box": None}:
        errors.append("defaults: roi must be auto/null")
    if canonical.get("engine_preference") != ["vision", "paddle"]:
        errors.append("defaults: engine_preference must be ['vision', 'paddle']")
    if "mock" not in canonical.get("explicit_only_engines", []):
        errors.append("defaults: mock must be explicit-only")
    if "mock" in canonical.get("engine_preference", []):
        errors.append("defaults: mock must not be auto-selected")


def check_roi(errors: list[str]) -> int:
    cases = load(FIXTURES / "roi-cases.json")
    for case in cases:
        source = case["source"]
        actual = convert_roi(
            case["normalized"], int(source["width"]), int(source["height"])
        )
        if actual != case["expected_pixels"]:
            errors.append(f"roi {case['name']}: expected {case['expected_pixels']}, got {actual}")
    return len(cases)


def check_srt(errors: list[str]) -> int:
    cases = load(FIXTURES / "srt-cases.json")
    for case in cases:
        actual = format_srt(case["entries"])
        if actual != case["expected"]:
            errors.append(f"srt {case['name']}: expected {case['expected']!r}, got {actual!r}")
    return len(cases)


def check_output(errors: list[str]) -> int:
    cases = load(FIXTURES / "output-cases.json")
    for case in cases:
        status, saved_name = plan_output(
            case["target_name"], case.get("existing_names", []), case["policy"]
        )
        expected = case["expected"]
        if status != expected["status"] or saved_name != expected["saved_name"]:
            errors.append(
                f"output {case['name']}: expected {expected}, got "
                f"{{'status': {status!r}, 'saved_name': {saved_name!r}}}"
            )
    return len(cases)


def check_statuses(errors: list[str]) -> None:
    data = load(FIXTURES / "statuses.json")
    if data.get("job") != JOB_STATUSES:
        errors.append("statuses: job list does not match the frozen contract")
    if data.get("task") != TASK_STATUSES:
        errors.append("statuses: task list does not match the frozen contract")
    mapping = data.get("mapping", {})
    if set(mapping.keys()) != set(TASK_STATUSES):
        errors.append("statuses: mapping keys must cover every task status")
    for key, value in mapping.items():
        if value is not None and value not in JOB_STATUSES:
            errors.append(f"statuses: mapping {key} -> {value} is not a job status")
    if mapping.get("exporting") != "finalizing":
        errors.append("statuses: exporting must map to finalizing")
    if mapping.get("skipped") is not None:
        errors.append("statuses: skipped must have no job state")


def check_schema(errors: list[str], warnings: list[str]) -> str:
    try:
        from jsonschema import Draft202012Validator  # type: ignore
    except Exception:
        warnings.append("jsonschema not installed; schema validation skipped")
        return "skipped"

    def validate(schema_name: str, fixture_name: str, items: Any) -> None:
        schema = load(SCHEMAS / schema_name)
        validator = Draft202012Validator(schema)
        for index, item in enumerate(items):
            for err in validator.iter_errors(item):
                errors.append(
                    f"schema {fixture_name}[{index}]: {list(err.path)} {err.message[:200]}"
                )

    extraction_cases = load(FIXTURES / "extraction-cases.json")
    validate(
        "extraction.schema.json",
        "extraction-cases.json",
        [case["spec"] for case in extraction_cases],
    )
    validate("job-event.schema.json", "job-events.json", load(FIXTURES / "job-events.json"))
    validate(
        "job-snapshot.schema.json",
        "job-snapshots.json",
        load(FIXTURES / "job-snapshots.json"),
    )
    validate("save-result.schema.json", "save-results.json", load(FIXTURES / "save-results.json"))
    return "OK"


def main() -> int:
    errors: list[str] = []
    warnings: list[str] = []

    version_path = CONTRACTS / "VERSION"
    try:
        contract_version = int(version_path.read_text(encoding="utf-8").strip())
        if contract_version < 1:
            errors.append("contracts/VERSION must be >= 1")
    except Exception as exc:
        errors.append(f"contracts/VERSION invalid: {exc}")

    for path in sorted(FIXTURES.glob("*.json")):
        try:
            load(path)
        except Exception as exc:
            errors.append(f"invalid JSON: {path.relative_to(ROOT)}: {exc}")

    check_defaults(errors)
    roi_count = check_roi(errors)
    srt_count = check_srt(errors)
    output_count = check_output(errors)
    check_statuses(errors)
    schema_result = check_schema(errors, warnings)

    print(
        f"SubLift contracts: {roi_count} ROI, {srt_count} SRT, {output_count} output cases; "
        f"schema {schema_result}"
    )
    for warning in warnings:
        print(f"WARN: {warning}")
    for error in errors:
        print(f"FAIL: {error}")
    if errors:
        print(f"result: FAIL ({len(errors)} error(s), {len(warnings)} warning(s))")
        return 1
    print(f"result: OK ({len(warnings)} warning(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main())
