#!/usr/bin/env python3
"""Validate canonical capability results before a platform hook accepts them.

This file is intentionally dependency-free and owns result-level protocol
validation.  State transitions remain in ``validate-orchestration.py``.
"""

from __future__ import annotations

import argparse
import json
import sys
from typing import Any


REVIEW_REQUIRED = {
    "schema_version",
    "feature_state_ref",
    "feature_id",
    "feature_plan",
    "review_round",
    "status",
    "review_target",
    "testing_result_ref",
    "tested_change_fingerprint",
    "reviewed_change_fingerprint",
    "reviewed_dimensions",
    "findings",
    "uncertainties",
    "review_summary",
    "review_evidence",
    "next_action",
    "execution_mode",
    "delegation_policy",
    "capability_run_ref",
    "tested_evidence_fingerprint",
    "reviewed_evidence_fingerprint",
}
REVIEW_DIMENSIONS = {
    "correctness",
    "requirements",
    "architecture-drift",
    "security",
    "reliability",
    "compatibility",
    "testing-gap",
}
FINDING_REQUIRED = {
    "finding_key",
    "priority",
    "blocking",
    "origin",
    "change_relation",
    "category",
    "confidence",
    "location",
    "summary",
    "trigger",
    "impact",
    "evidence",
    "protection_gap",
    "required_action",
    "lifecycle",
    "prior_finding_ref",
    "lifecycle_evidence",
    "lifecycle_reason",
}
UNCERTAINTY_REQUIRED = {"blocking", "question", "reason", "required_input", "related_finding_ref"}


def validate_fingerprint(value: object, name: str) -> str | None:
    if not isinstance(value, dict):
        return f"{name} 必须为对象"
    if set(value) != {"schema_version", "algorithm", "value", "entries"}:
        return f"{name} 字段不符合 change-snapshot Schema"
    if value.get("schema_version") != 1 or value.get("algorithm") != "sha256-manifest-v1":
        return f"{name} schema_version/algorithm 非法"
    fingerprint = value.get("value")
    if not isinstance(fingerprint, str) or len(fingerprint) != 64 or any(char not in "0123456789abcdef" for char in fingerprint):
        return f"{name}.value 不是小写 SHA-256"
    entries = value.get("entries")
    if not isinstance(entries, list) or not entries:
        return f"{name}.entries 不能为空"
    for entry in entries:
        if not isinstance(entry, dict) or set(entry) != {"path", "state", "content_sha256"}:
            return f"{name}.entries 项字段非法"
        path = entry.get("path")
        if not isinstance(path, str) or not path or path.startswith("/") or any(char in path for char in "\t\r\n"):
            return f"{name}.entries.path 非法"
        state = entry.get("state")
        digest = entry.get("content_sha256")
        if state not in {"present", "deleted"}:
            return f"{name}.entries.state 非法"
        if state == "deleted" and digest is not None:
            return f"{name}.entries.deleted 必须使用 null digest"
        if state == "present" and (not isinstance(digest, str) or len(digest) != 64 or any(char not in "0123456789abcdef" for char in digest)):
            return f"{name}.entries.present digest 非法"
    return None


def validate_evidence_fingerprint(value: object, name: str) -> str | None:
    if not isinstance(value, dict) or set(value) != {"schema_version", "algorithm", "value"}:
        return f"{name} 字段不符合 test-evidence-snapshot Schema"
    if value.get("schema_version") != 1 or value.get("algorithm") != "sha256-artifact-manifest-v1":
        return f"{name} schema_version/algorithm 非法"
    digest = value.get("value")
    if not isinstance(digest, str) or len(digest) != 64 or any(char not in "0123456789abcdef" for char in digest):
        return f"{name}.value 不是小写 SHA-256"
    return None


def validate_finding(value: object) -> str | None:
    if not isinstance(value, dict) or not FINDING_REQUIRED.issubset(value) or set(value) - FINDING_REQUIRED:
        return "Review finding 字段不完整或包含未知字段"
    if value.get("priority") not in {"P0", "P1", "P2", "P3"}:
        return "Review finding priority 非法"
    if not isinstance(value.get("blocking"), bool):
        return "Review finding blocking 必须为 bool"
    origin = value.get("origin")
    relation = value.get("change_relation")
    if origin not in {"introduced", "pre-existing"}:
        return "Review finding origin 非法"
    if origin == "introduced" and relation != "caused-by-change":
        return "introduced finding 必须使用 caused-by-change"
    if origin == "pre-existing" and relation not in {"worsened-by-change", "exposed-by-change", "required-by-change"}:
        return "pre-existing finding 的 change_relation 非法"
    if value.get("category") not in REVIEW_DIMENSIONS:
        return "Review finding category 非法"
    confidence = value.get("confidence")
    if isinstance(confidence, bool) or not isinstance(confidence, (int, float)) or not 0.8 <= confidence <= 1:
        return "Review finding confidence 必须在 [0.8, 1]"
    location = value.get("location")
    if (
        not isinstance(location, dict)
        or set(location) - {"path", "line", "symbol"}
        or not isinstance(location.get("path"), str)
        or not location["path"]
        or not isinstance(location.get("line"), int)
        or isinstance(location.get("line"), bool)
        or location["line"] < 1
    ):
        return "Review finding location 非法"
    for key in ("summary", "trigger", "impact", "protection_gap", "required_action", "lifecycle_reason"):
        if not isinstance(value.get(key), str) or not value[key].strip():
            return f"Review finding {key} 必须为非空字符串"
    if value.get("lifecycle") not in {"new", "unchanged", "worsened", "resolved", "not-reproduced"}:
        return "Review finding lifecycle 非法"
    prior = value.get("prior_finding_ref")
    if value["lifecycle"] == "new" and prior is not None:
        return "new finding 的 prior_finding_ref 必须为 null"
    if value["lifecycle"] != "new" and (not isinstance(prior, str) or not prior):
        return "重审 finding 必须带 prior_finding_ref"
    if value["lifecycle"] in {"resolved", "not-reproduced"} and value["blocking"]:
        return "resolved/not-reproduced finding 不能 blocking"
    for key in ("evidence", "lifecycle_evidence"):
        items = value.get(key)
        if not isinstance(items, list) or not items or any(not isinstance(item, str) or not item.strip() for item in items):
            return f"Review finding {key} 必须为非空字符串数组"
    return None


def validate_uncertainty(value: object) -> str | None:
    if not isinstance(value, dict) or set(value) != UNCERTAINTY_REQUIRED:
        return "Review uncertainty 字段不符合 Schema"
    if not isinstance(value.get("blocking"), bool):
        return "Review uncertainty blocking 必须为 bool"
    for key in ("question", "reason", "required_input"):
        if not isinstance(value.get(key), str) or not value[key].strip():
            return f"Review uncertainty {key} 必须为非空字符串"
    if value.get("related_finding_ref") is not None and (not isinstance(value["related_finding_ref"], str) or not value["related_finding_ref"]):
        return "Review uncertainty related_finding_ref 非法"
    return None


def validate_review_target(value: object) -> str | None:
    if not isinstance(value, dict):
        return "Review target 必须为对象"
    if set(value) != {"kind", "base_ref", "head_ref", "fingerprint"}:
        return "Review target 字段不符合 Schema"
    kind = value.get("kind")
    if kind not in {"working-tree", "staged", "commit", "range"}:
        return "Review target.kind 非法"
    if kind in {"working-tree", "staged"} and (not isinstance(value.get("base_ref"), str) or not value.get("base_ref") or value.get("head_ref") is not None):
        return "working-tree/staged Review target 引用非法"
    if kind in {"commit", "range"} and (not isinstance(value.get("base_ref"), str) or not value.get("base_ref") or not isinstance(value.get("head_ref"), str) or not value.get("head_ref")):
        return "commit/range Review target 引用非法"
    return validate_fingerprint(value.get("fingerprint"), "review_target.fingerprint")


def validate_review_result(value: object) -> str | None:
    if not isinstance(value, dict):
        return "Review Result 必须为 object"
    if set(value) != REVIEW_REQUIRED:
        missing = sorted(REVIEW_REQUIRED - set(value))
        extra = sorted(set(value) - REVIEW_REQUIRED)
        return f"Review Result 顶层字段不符合 Schema（missing={missing}, extra={extra}）"
    if value.get("schema_version") != 3:
        return "Review Result schema_version 必须为 3"
    status = value.get("status")
    if status not in {"REVIEW_PASSED", "CHANGES_REQUIRED", "REVIEW_BLOCKED"}:
        return "Review Result status 非法"
    if value.get("review_round") is not None and (not isinstance(value.get("review_round"), int) or isinstance(value.get("review_round"), bool) or value["review_round"] < 1):
        return "Review Result review_round 非法"
    if value.get("delegation_policy") not in {"required", "preferred", "unavailable", None}:
        return "Review Result delegation_policy 非法"
    if value.get("execution_mode") not in {"reviewer-agent", "inline-fallback", None}:
        return "Review Result execution_mode 非法"
    if value.get("capability_run_ref") is not None and (not isinstance(value["capability_run_ref"], str) or not value["capability_run_ref"]):
        return "Review Result capability_run_ref 非法"
    dimensions = value.get("reviewed_dimensions")
    if not isinstance(dimensions, list) or any(not isinstance(item, str) for item in dimensions) or len(set(dimensions)) != len(dimensions) or any(item not in REVIEW_DIMENSIONS for item in dimensions):
        return "Review Result reviewed_dimensions 非法"
    if value.get("review_target") is not None:
        target_error = validate_review_target(value["review_target"])
        if target_error:
            return target_error
    for key in ("tested_change_fingerprint", "reviewed_change_fingerprint"):
        if value.get(key) is not None:
            fingerprint_error = validate_fingerprint(value[key], key)
            if fingerprint_error:
                return fingerprint_error
    for key in ("tested_evidence_fingerprint", "reviewed_evidence_fingerprint"):
        if value.get(key) is not None:
            evidence_error = validate_evidence_fingerprint(value[key], key)
            if evidence_error:
                return evidence_error
    if status in {"REVIEW_PASSED", "CHANGES_REQUIRED"}:
        if value.get("delegation_policy") != "required" or value.get("execution_mode") != "reviewer-agent":
            return "required Reviewer 必须报告 delegation_policy=required 和 execution_mode=reviewer-agent"
        if not isinstance(value.get("capability_run_ref"), str) or not value["capability_run_ref"]:
            return "通过或要求修改的 Review 缺少 capability_run_ref"
        if not isinstance(value.get("review_round"), int) or value["review_round"] < 1:
            return "通过或要求修改的 Review 缺少合法 review_round"
        if len(value["reviewed_dimensions"]) != 7 or set(value["reviewed_dimensions"]) != REVIEW_DIMENSIONS:
            return "Review 未覆盖固定七个维度"
        if not isinstance(value.get("testing_result_ref"), str) or not value["testing_result_ref"]:
            return "通过或要求修改的 Review 缺少 testing_result_ref"
        expected_action = "ready-for-verification" if status == "REVIEW_PASSED" else "fix-and-retest"
        if value.get("next_action") != expected_action:
            return f"{status} 的 next_action 必须为 {expected_action}"
        if value.get("review_target") is None:
            return "通过或要求修改的 Review 缺少 review_target"
        for key in ("tested_change_fingerprint", "reviewed_change_fingerprint", "tested_evidence_fingerprint", "reviewed_evidence_fingerprint"):
            if value.get(key) is None:
                return f"通过或要求修改的 Review 缺少 {key} 快照"
    elif value.get("next_action") not in {"provide-input", "stabilize-target", "stop-review-loop"}:
        return "REVIEW_BLOCKED 的 next_action 非法"
    for key in ("feature_state_ref", "feature_id", "feature_plan", "review_summary"):
        if not isinstance(value.get(key), str) or not value[key].strip():
            return f"Review Result 缺少非空 {key}"
    if not isinstance(value.get("findings"), list) or not isinstance(value.get("uncertainties"), list):
        return "Review Result findings/uncertainties 必须为数组"
    for finding in value["findings"]:
        finding_error = validate_finding(finding)
        if finding_error:
            return finding_error
    for uncertainty in value["uncertainties"]:
        uncertainty_error = validate_uncertainty(uncertainty)
        if uncertainty_error:
            return uncertainty_error
    review_evidence = value.get("review_evidence")
    if not isinstance(review_evidence, list) or not review_evidence or any(not isinstance(item, str) or not item.strip() for item in review_evidence):
        return "Review review_evidence 必须为非空字符串数组"
    if status in {"REVIEW_PASSED", "CHANGES_REQUIRED"} and any(item.get("blocking") is True for item in value["uncertainties"] if isinstance(item, dict)):
        return "通过或要求修改的 Review 不能包含 blocking uncertainty"
    if status == "REVIEW_PASSED" and any(item.get("blocking") is True for item in value["findings"] if isinstance(item, dict)):
        return "REVIEW_PASSED 不能包含 blocking finding"
    if status == "CHANGES_REQUIRED" and not any(
        isinstance(item, dict) and item.get("blocking") is True and item.get("lifecycle") in {"new", "unchanged", "worsened"}
        for item in value["findings"]
    ):
        return "CHANGES_REQUIRED 必须包含开放的 blocking finding"
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate a canonical Harness result")
    parser.add_argument("kind", choices=["review"])
    args = parser.parse_args()
    try:
        value = json.load(sys.stdin)
    except (OSError, json.JSONDecodeError) as exc:
        print(f"结果 JSON 无法解析: {exc}", file=sys.stderr)
        return 2
    error = validate_review_result(value) if args.kind == "review" else "不支持的结果类型"
    if error:
        print(error, file=sys.stderr)
        return 2
    print("OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
