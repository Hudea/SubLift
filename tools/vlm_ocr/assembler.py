"""Pure deterministic schema validation, slot alignment, merging, and SRT assembly."""

from __future__ import annotations

import json
import re
from pathlib import Path
from typing import Any

from .models import (
    PLACEHOLDER_CATEGORY,
    SUBTITLE_CATEGORIES,
    AssembledCue,
    TimingSlot,
    TranscribedSlot,
)


SCHEMA_PATH = Path(__file__).resolve().with_name("schema.json")
_THINK_BLOCK_RE = re.compile(r"<think>.*?</think>", re.DOTALL | re.IGNORECASE)
_FENCE_START_RE = re.compile(r"^```(?:json)?\s*", re.IGNORECASE)
_FENCE_END_RE = re.compile(r"\s*```$")
_CJK_RE = re.compile(r"[\u4e00-\u9fff\u3400-\u4dbf\u3000-\u303f\uff00-\uffef]")


def load_slots_schema() -> dict[str, Any]:
    """Load formal Draft 2020-12 JSON Schema contract from schema.json."""
    data = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise ValueError(f"Invalid schema file at {SCHEMA_PATH}")
    return data


def extract_json_object(raw_text: str) -> dict[str, Any]:
    """Extract top-level JSON object from model output, stripping <think> and fences."""
    cleaned = _THINK_BLOCK_RE.sub("", raw_text).strip()
    if cleaned.startswith("```"):
        cleaned = _FENCE_START_RE.sub("", cleaned)
        cleaned = _FENCE_END_RE.sub("", cleaned).strip()

    try:
        parsed = json.loads(cleaned)
    except json.JSONDecodeError:
        start = cleaned.find("{")
        end = cleaned.rfind("}")
        if start < 0 or end <= start:
            raise ValueError(f"No JSON object found in model output: {cleaned[:160]!r}")
        parsed = json.loads(cleaned[start : end + 1])

    if not isinstance(parsed, dict):
        raise ValueError("Top-level JSON response must be an object")
    return parsed


def validate_payload_against_schema(
    raw_payload: dict[str, Any],
    schema: dict[str, Any] | None = None,
    *,
    allow_placeholder: bool = False,
) -> None:
    """Strictly validate VLM JSON payload against schema.json."""
    active_schema = schema or load_slots_schema()
    if not isinstance(raw_payload, dict):
        raise ValueError("VLM response JSON must be an object containing key 'slots'")

    required_top = set(active_schema.get("required", ["slots"]))
    if not required_top.issubset(raw_payload.keys()):
        raise ValueError("VLM response JSON missing required key 'slots'")
    if active_schema.get("additionalProperties") is False:
        allowed_top = set(active_schema.get("properties", {}).keys())
        extra_top = set(raw_payload.keys()) - allowed_top
        if extra_top:
            raise ValueError(f"Unexpected top-level keys in VLM JSON: {sorted(extra_top)}")

    raw_slots = raw_payload["slots"]
    if not isinstance(raw_slots, list):
        raise ValueError("'slots' field must be a list")

    item_schema = active_schema["properties"]["slots"]["items"]
    required_item = set(item_schema.get("required", ["id", "category", "lines"]))
    allowed_item = set(item_schema.get("properties", {}).keys())
    min_id = int(item_schema["properties"]["id"].get("minimum", 1))
    allowed_cats = frozenset(item_schema["properties"]["category"]["enum"])
    if allow_placeholder:
        allowed_cats = allowed_cats | {PLACEHOLDER_CATEGORY}

    for idx, item in enumerate(raw_slots):
        if not isinstance(item, dict):
            raise ValueError(f"Slot entry at index {idx} is not an object")
        if not required_item.issubset(item.keys()):
            raise ValueError(f"Slot entry at index {idx} missing required keys (id/category/lines)")
        if item_schema.get("additionalProperties") is False:
            extra_item = set(item.keys()) - allowed_item
            if extra_item:
                raise ValueError(
                    f"Slot entry at index {idx} has unexpected keys: {sorted(extra_item)}"
                )
        raw_id = item["id"]
        if not isinstance(raw_id, int) or isinstance(raw_id, bool) or raw_id < min_id:
            raise ValueError(
                f"Invalid slot 'id' {raw_id!r} at index {idx}: must be an integer >= {min_id}"
            )
        raw_cat = item["category"]
        if not isinstance(raw_cat, str) or raw_cat.strip() not in allowed_cats:
            raise ValueError(f"Invalid category {raw_cat!r} at slot #{raw_id}")
        raw_lines = item["lines"]
        if not isinstance(raw_lines, list):
            raise ValueError(f"'lines' must be a list at slot #{raw_id}")
        for line_idx, ln in enumerate(raw_lines):
            if not isinstance(ln, str):
                raise ValueError(
                    f"Line #{line_idx} at slot #{raw_id} must be a string, got {type(ln).__name__}"
                )


def normalize_subtitle_line(line: str, script: str = "auto") -> str:
    """Normalize punctuation while preserving single internal spaces."""
    cleaned = line.strip()
    if not cleaned:
        return ""
    cleaned = re.sub(r"\.{3,}|…{2,}", "…", cleaned)
    use_cjk_punct = script == "cjk" or (script == "auto" and bool(_CJK_RE.search(cleaned)))
    if use_cjk_punct:
        cleaned = cleaned.replace("(", "（").replace(")", "）")
        cleaned = cleaned.replace("?", "？").replace("!", "！")
        cleaned = re.sub(r"（\s+", "（", cleaned)
        cleaned = re.sub(r"\s+）", "）", cleaned)
    cleaned = re.sub(r" {2,}", " ", cleaned)
    return cleaned


def validate_and_align_sheet_slots(
    raw_payload: dict[str, Any],
    expected_slot_ids: list[int],
    *,
    script: str = "auto",
    allow_placeholder: bool = False,
) -> tuple[list[TranscribedSlot], bool]:
    """Validate payload and align slots to `expected_slot_ids` (recovering local 1..K numbering)."""
    validate_payload_against_schema(raw_payload, allow_placeholder=allow_placeholder)
    raw_slots: list[dict[str, Any]] = raw_payload["slots"]
    if len(raw_slots) != len(expected_slot_ids):
        raise ValueError(
            f"Slot count mismatch: expected {len(expected_slot_ids)} slots "
            f"{expected_slot_ids}, got {len(raw_slots)}"
        )

    returned_ids = [int(item["id"]) for item in raw_slots]
    remapped = False

    if returned_ids == expected_slot_ids:
        aligned = list(zip(expected_slot_ids, raw_slots, strict=True))
    elif returned_ids == list(range(1, len(expected_slot_ids) + 1)):
        remapped = True
        aligned = list(zip(expected_slot_ids, raw_slots, strict=True))
    elif len(set(returned_ids)) == len(returned_ids) and set(returned_ids) == set(expected_slot_ids):
        by_id = {int(item["id"]): item for item in raw_slots}
        aligned = [(sid, by_id[sid]) for sid in expected_slot_ids]
    else:
        raise ValueError(
            f"Returned slot IDs {returned_ids} do not match expected {expected_slot_ids}"
        )

    validated: list[TranscribedSlot] = []
    for sid, item in aligned:
        cat = item["category"].strip()
        if cat == "empty":
            norm_lines: list[str] = []
        else:
            norm_lines = [normalize_subtitle_line(ln, script=script) for ln in item["lines"]]
            norm_lines = [ln for ln in norm_lines if ln]
        validated.append(TranscribedSlot(slot_id=sid, category=cat, lines=norm_lines))
    return validated, remapped


def merge_adjacent_cues(
    cues: list[AssembledCue],
    *,
    max_gap_ms: int = 200,
    blocked_merge_slot_ids: set[int] | None = None,
) -> tuple[list[AssembledCue], list[dict[str, Any]]]:
    """Merge consecutive cues with identical text & category within `max_gap_ms` unless blocked."""
    if not cues:
        return [], []

    blocked = blocked_merge_slot_ids or set()
    merged: list[AssembledCue] = []
    merged_groups: list[dict[str, Any]] = []

    for cue in cues:
        if merged:
            prev = merged[-1]
            gap_ms = cue.start_ms - prev.end_ms
            if (
                cue.slot_id not in blocked
                and prev.text == cue.text
                and bool(prev.text)
                and prev.category == cue.category
                and -max_gap_ms <= gap_ms <= max_gap_ms
            ):
                new_start = min(prev.start_ms, cue.start_ms)
                new_end = max(prev.end_ms, cue.end_ms)
                merged[-1] = AssembledCue(
                    cue_index=prev.cue_index,
                    slot_id=prev.slot_id,
                    start_ms=new_start,
                    end_ms=new_end,
                    category=prev.category,
                    text=prev.text,
                )
                if merged_groups and merged_groups[-1]["primary_slot_id"] == prev.slot_id:
                    merged_groups[-1]["merged_slot_ids"].append(cue.slot_id)
                    merged_groups[-1]["end_ms"] = new_end
                else:
                    merged_groups.append(
                        {
                            "primary_slot_id": prev.slot_id,
                            "merged_slot_ids": [prev.slot_id, cue.slot_id],
                            "start_ms": new_start,
                            "end_ms": new_end,
                            "category": prev.category,
                            "text": prev.text,
                        }
                    )
                continue

        merged.append(
            AssembledCue(
                cue_index=len(merged) + 1,
                slot_id=cue.slot_id,
                start_ms=cue.start_ms,
                end_ms=cue.end_ms,
                category=cue.category,
                text=cue.text,
            )
        )
    return merged, merged_groups


def assemble_cues_from_slots(
    timing_slots: list[TimingSlot],
    transcribed_slots: list[TranscribedSlot],
    *,
    merge_adjacent: bool = True,
    max_merge_gap_ms: int = 200,
) -> tuple[list[AssembledCue], list[dict[str, Any]], list[dict[str, Any]]]:
    """Deterministically assemble TimingSlots + TranscribedSlots into cues."""
    slot_map: dict[int, TranscribedSlot] = {}
    for t_slot in transcribed_slots:
        if t_slot.slot_id in slot_map:
            raise ValueError(f"Duplicate transcribed slot_id #{t_slot.slot_id}")
        slot_map[t_slot.slot_id] = t_slot

    missing = [ts.slot_id for ts in timing_slots if ts.slot_id not in slot_map]
    if missing:
        raise ValueError(f"Missing transcribed slots for timing slot IDs: {missing}")

    raw_cues: list[AssembledCue] = []
    filtered_noise: list[dict[str, Any]] = []
    blocked_merge_slot_ids: set[int] = set()
    saw_intervening_non_subtitle = False

    for ts in timing_slots:
        t_slot = slot_map[ts.slot_id]
        is_placeholder = t_slot.category == PLACEHOLDER_CATEGORY and bool(t_slot.lines)
        if (t_slot.category not in SUBTITLE_CATEGORIES and not is_placeholder) or not t_slot.lines:
            filtered_noise.append(
                {
                    "slot_id": ts.slot_id,
                    "start_ms": ts.start_ms,
                    "end_ms": ts.end_ms,
                    "category": t_slot.category,
                    "lines": t_slot.lines,
                }
            )
            saw_intervening_non_subtitle = True
            continue

        if is_placeholder:
            blocked_merge_slot_ids.add(ts.slot_id)
            raw_cues.append(
                AssembledCue(
                    cue_index=len(raw_cues) + 1,
                    slot_id=ts.slot_id,
                    start_ms=ts.start_ms,
                    end_ms=ts.end_ms,
                    category=t_slot.category,
                    text=t_slot.text,
                )
            )
            saw_intervening_non_subtitle = True
            continue

        if saw_intervening_non_subtitle:
            blocked_merge_slot_ids.add(ts.slot_id)
        saw_intervening_non_subtitle = False
        raw_cues.append(
            AssembledCue(
                cue_index=len(raw_cues) + 1,
                slot_id=ts.slot_id,
                start_ms=ts.start_ms,
                end_ms=ts.end_ms,
                category=t_slot.category,
                text=t_slot.text,
            )
        )

    if merge_adjacent:
        assembled, merged_groups = merge_adjacent_cues(
            raw_cues,
            max_gap_ms=max_merge_gap_ms,
            blocked_merge_slot_ids=blocked_merge_slot_ids,
        )
    else:
        assembled, merged_groups = raw_cues, []

    return assembled, filtered_noise, merged_groups


def format_srt_timestamp(ms: int) -> str:
    """Format milliseconds into SRT timestamp 'HH:MM:SS,mmm'."""
    clamped = max(0, int(ms))
    h, rem = divmod(clamped, 3_600_000)
    m, rem = divmod(rem, 60_000)
    s, millis = divmod(rem, 1_000)
    return f"{h:02d}:{m:02d}:{s:02d},{millis:03d}"


def format_srt(cues: list[AssembledCue]) -> str:
    """Serialize assembled cues into standard SRT string."""
    blocks = [
        f"{cue.cue_index}\n"
        f"{format_srt_timestamp(cue.start_ms)} --> {format_srt_timestamp(cue.end_ms)}\n"
        f"{cue.text}\n"
        for cue in cues
    ]
    return "\n".join(blocks)


def write_srt(cues: list[AssembledCue], out_path: Path) -> None:
    """Write assembled cues to `out_path` in UTF-8."""
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(format_srt(cues), encoding="utf-8")
