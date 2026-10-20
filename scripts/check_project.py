#!/usr/bin/env python3
"""SubLift project truth check (Phase index, status, links, optional JSON Schema).

This is a continuity gate, not a product test. It must stay dependency-light:
stdlib by default, full JSON Schema only when jsonschema is importable.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parents[1]

PHASE_STATUSES = {"not-started", "in-progress", "ready-for-merge", "blocked", "done"}
DELIVERABLE_STATUSES = {"not-started", "in-progress", "blocked", "done", "archived", "completed"}

REQUIRED_FILES = [
    "AGENTS.md",
    "README.md",
    "progress.md",
    "phases.json",
    "phases.schema.json",
    "docs/TESTMAP.md",
    "docs/README.md",
    "docs/phases/phaseN.schema.json",
]

CURRENT_DOC_GLOBS = [
    "README.md",
    "AGENTS.md",
    "progress.md",
    "docs/README.md",
    "docs/ARCHITECTURE.md",
    "docs/REQUIREMENTS.md",
    "docs/DEVELOPMENT.md",
    "docs/TESTMAP.md",
    "docs/HURDLES.md",
    "docs/DECISIONS.md",
    "docs/cpp/*.md",
    "docs/design_ui/*.md",
    "benchmark/README.md",
    "benchmark/baselines/*.md",
    "apps/*/README.md",
]

LINK_RE = re.compile(r"\[([^\]]*)\]\((<?)([^)\s>]+)>?((#[^)]*)?)\)")


def load_json(path: Path) -> Any:
    with path.open("r", encoding="utf-8") as fh:
        return json.load(fh)


def phase_number(phase_id: str) -> str:
    m = re.search(r"(\d+(?:\.\d+)?)", phase_id)
    return m.group(1) if m else phase_id


def strip_code_blocks(text: str) -> str:
    lines = []
    in_fence = False
    for line in text.splitlines():
        if line.lstrip().startswith("```"):
            in_fence = not in_fence
            continue
        if not in_fence:
            lines.append(line)
    return "\n".join(lines)


def check_links(errors: list[str], warnings: list[str]) -> int:
    checked = 0
    files: list[Path] = []
    for pattern in CURRENT_DOC_GLOBS:
        files.extend(sorted(ROOT.glob(pattern)))
    seen = set()
    for file in files:
        if file in seen or not file.is_file():
            continue
        seen.add(file)
        text = strip_code_blocks(file.read_text(encoding="utf-8"))
        for match in LINK_RE.finditer(text):
            target = match.group(3).strip()
            if not target or target.startswith(("#", "http://", "https://", "mailto:")):
                continue
            target = target.split("#", 1)[0]
            if not target:
                continue
            if any(ch in target for ch in "*?{}") or target.startswith("<"):
                continue
            checked += 1
            resolved = (file.parent / unquote(target)).resolve()
            if not resolved.exists():
                errors.append(f"broken link: {file.relative_to(ROOT)} -> {target}")
    return checked


def check_schema(errors: list[str], warnings: list[str]) -> str:
    try:
        from jsonschema import Draft202012Validator  # type: ignore
    except Exception:
        warnings.append("jsonschema not installed; full JSON Schema validation skipped")
        return "skipped"
    index_schema = load_json(ROOT / "phases.schema.json")
    detail_schema = load_json(ROOT / "docs/phases/phaseN.schema.json")
    index = load_json(ROOT / "phases.json")
    validators = [
        (Path("phases.json"), Draft202012Validator(index_schema), index),
    ]
    for phase in index.get("phases", []):
        detail_file = phase.get("detail_file")
        if not detail_file:
            continue
        detail_path = ROOT / detail_file
        if detail_path.is_file():
            validators.append(
                (Path(detail_file), Draft202012Validator(detail_schema), load_json(detail_path))
            )
    count = 0
    for label, validator, instance in validators:
        count += 1
        for err in sorted(validator.iter_errors(instance), key=lambda e: list(e.path)):
            errors.append(f"schema {label}: {list(err.path)} {err.message[:240]}")
    return f"OK ({count} documents)"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--no-schema", action="store_true", help="skip JSON Schema validation")
    args = parser.parse_args()

    errors: list[str] = []
    warnings: list[str] = []

    for rel in REQUIRED_FILES:
        if not (ROOT / rel).is_file():
            errors.append(f"missing required file: {rel}")

    index_path = ROOT / "phases.json"
    if not index_path.is_file():
        print("FAIL: phases.json missing")
        return 1

    try:
        index = load_json(index_path)
    except Exception as exc:
        print(f"FAIL: phases.json invalid JSON: {exc}")
        return 1

    phases = index.get("phases")
    if not isinstance(phases, list) or not phases:
        print("FAIL: phases.json has no phases[]")
        return 1

    referenced: set[str] = set()
    ids: set[str] = set()
    active_like: list[str] = []
    in_progress_phases: list[str] = []

    for phase in phases:
        phase_id = str(phase.get("id", ""))
        status = str(phase.get("status", ""))
        detail_rel = str(phase.get("detail_file", ""))
        if not phase_id:
            errors.append("phase with empty id")
            continue
        if phase_id in ids:
            errors.append(f"duplicate phase id: {phase_id}")
        ids.add(phase_id)
        if status not in PHASE_STATUSES:
            errors.append(f"{phase_id}: invalid index status {status!r}")
        if status in {"in-progress", "ready-for-merge"}:
            active_like.append(phase_id)
        if status == "in-progress":
            in_progress_phases.append(phase_id)

        detail_path = ROOT / detail_rel
        if not detail_rel:
            errors.append(f"{phase_id}: missing detail_file")
            continue
        referenced.add(detail_rel)
        if not detail_path.is_file():
            errors.append(f"{phase_id}: detail_file not found: {detail_rel}")
            continue
        try:
            detail = load_json(detail_path)
        except Exception as exc:
            errors.append(f"{phase_id}: detail JSON invalid: {exc}")
            continue

        if detail.get("schema_version") == 2:
            detail_status = detail.get("status")
            if detail_status not in PHASE_STATUSES:
                errors.append(f"{phase_id}: invalid detail status {detail_status!r}")
            elif detail_status != status:
                errors.append(
                    f"{phase_id}: index status {status!r} != detail status {detail_status!r}"
                )

            deliverables = detail.get("deliverables")
            if not isinstance(deliverables, list):
                errors.append(f"{phase_id}: deliverables must be an array")
                continue
            if not 2 <= len(deliverables) <= 8:
                errors.append(f"{phase_id}: expected 2-8 deliverables, got {len(deliverables)}")

            d_ids: list[str] = []
            d_statuses: list[str] = []
            for pos, item in enumerate(deliverables, start=1):
                d_id = str(item.get("id", ""))
                d_status = str(item.get("status", ""))
                d_ids.append(d_id)
                d_statuses.append(d_status)
                if d_id != f"D{pos:02d}":
                    errors.append(f"{phase_id}: deliverable {pos} id {d_id!r} != D{pos:02d}")
                if d_status not in DELIVERABLE_STATUSES:
                    errors.append(f"{phase_id}/{d_id}: invalid status {d_status!r}")
                if d_status == "done" and not item.get("evidence"):
                    errors.append(f"{phase_id}/{d_id}: done deliverable has no evidence")

            if status in {"done", "ready-for-merge"} and any(s != "done" for s in d_statuses):
                errors.append(f"{phase_id}: {status} phase requires all deliverables done")
            if status == "in-progress" and d_statuses.count("in-progress") != 1:
                errors.append(
                    f"{phase_id}: in-progress phase requires "
                    "exactly one in-progress deliverable"
                )

    if len(in_progress_phases) > 1:
        errors.append("more than one phase is in-progress: " + ", ".join(in_progress_phases))

    for path in sorted((ROOT / "docs/phases").glob("phase[0-9]*.json")):
        rel = str(path.relative_to(ROOT))
        if rel not in referenced:
            warnings.append(f"orphan phase detail (not in phases.json): {rel}")

    progress_path = ROOT / "progress.md"
    if progress_path.is_file():
        progress_text = progress_path.read_text(encoding="utf-8")
        line_count = len(progress_text.splitlines())
        if line_count > 80:
            warnings.append(f"progress.md is {line_count} lines; keep it a dashboard (<80)")
        for phase_id in active_like:
            number = phase_number(phase_id)
            if f"Phase {number}" not in progress_text and phase_id not in progress_text:
                warnings.append(f"progress.md does not mention {phase_id}")

    for agent_file in sorted((ROOT / ".agent").rglob("*")):
        if agent_file.is_file() and agent_file.suffix in {".md", ".json"}:
            text = agent_file.read_text(encoding="utf-8", errors="ignore")
            if "phase0" in text:
                errors.append(f"stale phase0 reference: {agent_file.relative_to(ROOT)}")

    try:
        schema_text = (ROOT / "phases.schema.json").read_text(encoding="utf-8")
        if "agent-template.local" in schema_text:
            errors.append("phases.schema.json still uses agent-template.local $id")
    except OSError:
        pass

    links_checked = check_links(errors, warnings)
    schema_result = "skipped" if args.no_schema else check_schema(errors, warnings)

    print(
        f"SubLift project check: {len(phases)} phases, "
        f"{links_checked} links, schema {schema_result}"
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
