#!/usr/bin/env python3
"""Validate Harness orchestration semantics, not just JSON shape.

The project deliberately keeps this validator dependency-free. JSON Schema
checks structure; this file checks the evidence chain that permits a state
transition.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath
from typing import Any, Iterable


CAPABILITIES = {
    "feature-development",
    "phase-development",
    "plan",
    "test",
    "review",
    "verify",
    "commit",
    "commit-message",
}
ENTRYPOINTS = {
    "feature-development": ".agent/skills/develop-feature/SKILL.md",
    "phase-development": ".agent/skills/develop-phase/SKILL.md",
    "plan": ".agent/skills/plan/SKILL.md",
    "test": ".agent/skills/test/SKILL.md",
    "review": ".agent/skills/review/SKILL.md",
    "verify": ".agent/skills/verify/SKILL.md",
    "commit": ".agent/skills/commit/SKILL.md",
    "commit-message": ".agent/skills/commit-message/SKILL.md",
}
STAGES = {None, "prepare", "verify", "diagnose", "feature", "phase"}
FEATURE_STAGES = {
    "planning",
    "testing-prepare",
    "implementing",
    "testing-verify",
    "reviewing",
    "verifying",
    "committing",
    "completed",
    "blocked",
}
ALL_REVIEW_DIMENSIONS = {
    "correctness",
    "requirements",
    "architecture-drift",
    "security",
    "reliability",
    "compatibility",
    "testing-gap",
}
SHA_RE = re.compile(r"^[0-9a-fA-F]{7,64}$")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
CAPABILITY_SCHEMA_VERSION = 3
RUNTIME_PLATFORMS = {"opencode", "claude-code"}


class ValidationError(Exception):
    """A semantic validation failure."""


def fail(message: str) -> None:
    raise ValidationError(message)


def load_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        fail(f"文件不存在: {path}")
    except json.JSONDecodeError as exc:
        fail(f"JSON 无法解析: {path}: {exc}")
    raise AssertionError("unreachable")


def repo_root_for(path: Path) -> Path:
    candidate = path.resolve()
    if candidate.is_file():
        candidate = candidate.parent
    for parent in (candidate, *candidate.parents):
        if (parent / ".agent").is_dir():
            return parent
    return Path.cwd().resolve()


def state_ref_for(path: Path, repo_root: Path) -> str:
    try:
        return path.resolve().relative_to(repo_root).as_posix()
    except ValueError:
        return path.resolve().as_posix()


def git_output(repo_root: Path, *args: str) -> str:
    result = subprocess.run(
        ["git", *args],
        cwd=repo_root,
        text=True,
        capture_output=True,
        check=False,
    )
    if result.returncode != 0:
        detail = (result.stderr or result.stdout).strip()
        fail(f"Git 校验失败: {' '.join(args)}: {detail or 'unknown error'}")
    return result.stdout.strip()


def git_commit_sha(repo_root: Path, ref: str, label: str) -> str:
    value = ensure_nonempty_string(ref, label)
    return git_output(repo_root, "rev-parse", "--verify", f"{value}^{{commit}}")


def parse_timestamp(value: Any, label: str) -> datetime:
    if not isinstance(value, str) or not value:
        fail(f"{label} 必须是非空时间戳")
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
        return parsed if parsed.tzinfo is not None else parsed.replace(tzinfo=timezone.utc)
    except ValueError:
        fail(f"{label} 不是 ISO-8601 时间戳: {value}")
    raise AssertionError("unreachable")


def same_json(left: Any, right: Any) -> bool:
    return left == right


def ensure_object(value: Any, label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        fail(f"{label} 必须是 object")
    return value


def ensure_array(value: Any, label: str) -> list[Any]:
    if not isinstance(value, list):
        fail(f"{label} 必须是 array")
    return value


def ensure_nonempty_string(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value:
        fail(f"{label} 必须是非空字符串")
    return value


def validate_receipt(receipt: Any, index: int, run_ids: set[str]) -> dict[str, Any]:
    item = ensure_object(receipt, f"capability_runs[{index}]")
    required = {
        "schema_version",
        "run_id",
        "capability",
        "stage",
        "entrypoint",
        "delegation_policy",
        "executor",
        "input",
        "output",
        "started_at",
        "completed_at",
    }
    missing = sorted(required - item.keys())
    if missing:
        fail(f"capability_runs[{index}] 缺少字段: {', '.join(missing)}")
    if item["schema_version"] != CAPABILITY_SCHEMA_VERSION:
        fail(f"capability_runs[{index}] schema_version 必须为 {CAPABILITY_SCHEMA_VERSION}")
    run_id = ensure_nonempty_string(item["run_id"], f"capability_runs[{index}].run_id")
    if run_id in run_ids:
        fail(f"capability receipt run_id 重复: {run_id}")
    run_ids.add(run_id)
    capability = item["capability"]
    if capability not in CAPABILITIES:
        fail(f"capability_runs[{index}] 不支持的 capability: {capability}")
    if item["stage"] not in STAGES:
        fail(f"capability_runs[{index}] 不支持的 stage: {item['stage']}")
    entrypoint = ensure_nonempty_string(item["entrypoint"], f"capability_runs[{index}].entrypoint")
    if entrypoint != ENTRYPOINTS[capability]:
        fail(f"capability_runs[{index}] entrypoint 与 capability 不匹配: {entrypoint}")
    policy = item["delegation_policy"]
    if policy not in {"required", "preferred", "unavailable"}:
        fail(f"capability_runs[{index}] delegation_policy 非法: {policy}")

    executor = ensure_object(item["executor"], f"capability_runs[{index}].executor")
    for field in (
        "kind",
        "platform",
        "agent_name",
        "invocation_ref",
        "attestation_ref",
        "conversation_id",
        "parent_session_id",
        "call_id",
        "transcript_path",
    ):
        if field not in executor:
            fail(f"capability_runs[{index}].executor 缺少字段: {field}")
    kind = executor.get("kind")
    if kind not in {"main-agent", "subagent", "workflow", "inline-fallback"}:
        fail(f"capability_runs[{index}] executor.kind 非法: {kind}")
    if not isinstance(executor.get("platform"), str) or not executor["platform"]:
        fail(f"capability_runs[{index}].executor.platform 必须非空")
    if kind == "subagent":
        ensure_nonempty_string(executor.get("agent_name"), f"capability_runs[{index}].executor.agent_name")
        ensure_nonempty_string(executor.get("invocation_ref"), f"capability_runs[{index}].executor.invocation_ref")
    if kind == "workflow":
        ensure_nonempty_string(executor.get("invocation_ref"), f"capability_runs[{index}].executor.invocation_ref")
    if kind == "inline-fallback" and policy == "required":
        fail(f"capability_runs[{index}] required 委派不得使用 inline-fallback")
    for field in ("agent_name", "invocation_ref", "attestation_ref", "conversation_id", "parent_session_id", "call_id", "transcript_path"):
        value = executor.get(field)
        if value is not None and not isinstance(value, str):
            fail(f"capability_runs[{index}].executor.{field} 必须是字符串或 null")
    if capability in {"plan", "review"} and policy == "required" and item.get("output", {}).get("status") in {"running", "completed"}:
        if kind != "subagent" or not executor.get("invocation_ref"):
            fail(f"{capability} 在 delegation_policy=required 时必须有真实 subagent invocation_ref")
        if executor.get("platform") == "antigravity":
            expected_agent = "planner" if capability == "plan" else "reviewer"
            if executor.get("agent_name") != expected_agent:
                fail(f"Antigravity {capability} receipt 必须引用 {expected_agent} Subagent")

    if capability in {"feature-development", "phase-development"} and item.get("output", {}).get("status") in {"running", "completed"}:
        if kind != "workflow" or not executor.get("invocation_ref"):
            fail(f"{capability} receipt 必须引用真实 Workflow invocation_ref")
    if (
        executor.get("platform") == "antigravity"
        and kind in {"subagent", "workflow"}
        and item.get("output", {}).get("status") in {"running", "completed"}
    ):
        ensure_nonempty_string(executor.get("conversation_id"), f"capability_runs[{index}].executor.conversation_id")
        ensure_nonempty_string(executor.get("transcript_path"), f"capability_runs[{index}].executor.transcript_path")

    input_data = ensure_object(item["input"], f"capability_runs[{index}].input")
    if "state_ref" not in input_data or "fingerprint" not in input_data:
        fail(f"capability_runs[{index}].input 必须包含 state_ref 与 fingerprint")
    output = ensure_object(item["output"], f"capability_runs[{index}].output")
    status = output.get("status")
    if status not in {"running", "prepared", "completed", "failed", "blocked"}:
        fail(f"capability_runs[{index}].output.status 非法: {status}")
    if status == "completed":
        ensure_nonempty_string(output.get("result_ref"), f"capability_runs[{index}].output.result_ref")
        if item.get("completed_at") is None:
            fail(f"capability_runs[{index}] 非运行中 receipt 必须有 completed_at")
    elif status in {"failed", "blocked"} and item.get("completed_at") is None:
        fail(f"capability_runs[{index}] 非运行中 receipt 必须有 completed_at")
    elif status == "prepared":
        if capability != "commit":
            fail(f"只有 commit receipt 可以处于 prepared 状态")
        ensure_nonempty_string(output.get("result_ref"), f"capability_runs[{index}].output.result_ref")
        if item.get("completed_at") is not None:
            fail(f"prepared receipt 的 completed_at 必须为 null")
    elif item.get("completed_at") is not None:
        fail(f"running receipt 的 completed_at 必须为 null")
    result_sha256 = output.get("result_sha256")
    if result_sha256 is not None and (not isinstance(result_sha256, str) or not SHA256_RE.fullmatch(result_sha256)):
        fail(f"capability_runs[{index}].output.result_sha256 必须是小写 SHA-256 或 null")
    parse_timestamp(item["started_at"], f"capability_runs[{index}].started_at")
    if item.get("completed_at") is not None:
        completed_at = parse_timestamp(item["completed_at"], f"capability_runs[{index}].completed_at")
        if completed_at < parse_timestamp(item["started_at"], f"capability_runs[{index}].started_at"):
            fail(f"capability_runs[{index}] completed_at 早于 started_at")
    return item


def validate_receipts(state: dict[str, Any], repo_root: Path | None = None) -> dict[str, list[dict[str, Any]]]:
    receipts = ensure_array(state.get("capability_runs"), "capability_runs")
    by_capability: dict[str, list[dict[str, Any]]] = {capability: [] for capability in CAPABILITIES}
    run_ids: set[str] = set()
    for index, receipt in enumerate(receipts):
        item = validate_receipt(receipt, index, run_ids)
        if repo_root is not None:
            validate_runtime_invocation(item, repo_root)
        by_capability[item["capability"]].append(item)
    return by_capability


def ref_matches(receipt_ref: Any, state_ref: str, fragment: str) -> bool:
    if not isinstance(receipt_ref, str):
        return False
    normalized = receipt_ref.replace("\\", "/")
    return normalized in {f"{state_ref}#/{fragment}", f"./{state_ref}#/{fragment}", f"#/{fragment}"} or normalized.endswith(f"#/{fragment}")


def receipt_ref_for_state(receipt: dict[str, Any], state_ref: str) -> bool:
    value = receipt.get("input", {}).get("state_ref")
    if value is None:
        return True
    if not isinstance(value, str):
        return False
    normalized = value.replace("\\", "/")
    return normalized in {state_ref, f"./{state_ref}"} or normalized.endswith(f"/{state_ref}")


def normalize_ref(value: str) -> str:
    normalized = value.replace("\\", "/")
    while normalized.startswith("./"):
        normalized = normalized[2:]
    return normalized


def resolve_runtime_attestation(repo_root: Path, platform: str, raw_ref: Any) -> tuple[Path, str]:
    reference = normalize_ref(ensure_nonempty_string(raw_ref, f"{platform} receipt.executor.attestation_ref"))
    prefix = f".agent/state/runtime/{platform}/"
    parts = PurePosixPath(reference).parts
    if not reference.startswith(prefix) or ".." in parts or not parts[-1].endswith(".json"):
        fail(f"{platform} attestation_ref 必须指向平台 runtime JSON: {reference}")
    path = repo_root.joinpath(*parts)
    current = repo_root
    for part in parts:
        current /= part
        if current.is_symlink():
            fail(f"{platform} attestation 路径包含符号链接: {reference}")
    if not path.is_file():
        fail(f"缺少 {platform} attestation 文件: {reference}")
    return path, reference


def record_value(record: dict[str, Any], *keys: str) -> Any:
    for key in keys:
        value = record.get(key)
        if value is not None:
            return value
    return None


def validate_runtime_attestation(receipt: dict[str, Any], repo_root: Path) -> None:
    executor = receipt.get("executor", {})
    platform = executor.get("platform")
    kind = executor.get("kind")
    status = receipt.get("output", {}).get("status")
    if platform not in RUNTIME_PLATFORMS or kind not in {"subagent", "workflow"} or status not in {"running", "completed"}:
        return

    invocation_ref = ensure_nonempty_string(executor.get("invocation_ref"), f"{platform} receipt.executor.invocation_ref")
    attestation_path, attestation_ref = resolve_runtime_attestation(repo_root, platform, executor.get("attestation_ref"))
    if normalize_ref(invocation_ref) != attestation_ref:
        fail(f"{platform} invocation_ref 与 attestation_ref 不一致: {invocation_ref}")
    record = ensure_object(load_json(attestation_path), f"{platform} attestation")
    if record.get("platform") != platform:
        fail(f"{platform} attestation 的 platform 不匹配: {attestation_ref}")

    expected_agent = executor.get("agent_name")
    actual_agent = record_value(record, "agent_name", "agent_type")
    if expected_agent is not None and actual_agent != expected_agent:
        fail(f"{platform} attestation 的 Agent 不匹配: {attestation_ref}")

    expected_state = receipt.get("input", {}).get("state_ref")
    actual_state = record_value(record, "state_ref", "feature_state_ref")
    if expected_state != actual_state:
        fail(f"{platform} attestation 的 state_ref 不匹配: {attestation_ref}")

    actual_conversation = record_value(record, "conversation_id", "child_session_id", "agent_id", "session_id", "call_id")
    expected_conversation = executor.get("conversation_id")
    if status == "completed":
        expected_conversation = ensure_nonempty_string(expected_conversation, f"{platform} receipt.executor.conversation_id")
        if actual_conversation != expected_conversation:
            fail(f"{platform} attestation 的 conversation/session 不匹配: {attestation_ref}")
    elif expected_conversation is not None and actual_conversation != expected_conversation:
        fail(f"{platform} running attestation 的 conversation/session 不匹配: {attestation_ref}")

    expected_parent = executor.get("parent_session_id")
    if expected_parent is not None:
        actual_parent = record_value(record, "parent_session_id", "parent_session", "session_id")
        if actual_parent != expected_parent:
            fail(f"{platform} attestation 的 parent session 不匹配: {attestation_ref}")

    expected_call = executor.get("call_id")
    if expected_call is not None:
        actual_call = record_value(record, "call_id", "agent_id")
        if actual_call != expected_call:
            fail(f"{platform} attestation 的 call/agent ID 不匹配: {attestation_ref}")

    expected_transcript = executor.get("transcript_path")
    actual_transcript = record_value(record, "transcript_path", "agent_transcript_path")
    if expected_transcript is not None and actual_transcript != expected_transcript:
        fail(f"{platform} attestation 的 transcript_path 不匹配: {attestation_ref}")
    if actual_transcript is not None:
        transcript_path = Path(str(actual_transcript)).expanduser()
        if not transcript_path.is_absolute():
            transcript_path = repo_root / transcript_path
        if not transcript_path.is_file():
            fail(f"{platform} attestation 的 transcript 不存在: {actual_transcript}")

    actual_status = record.get("status")
    allowed_statuses = {"running", "started"} if status == "running" else {"completed"}
    if actual_status not in allowed_statuses:
        fail(f"{platform} receipt={status} 与 attestation status={actual_status} 不一致: {attestation_ref}")

    started_at = parse_timestamp(record.get("started_at"), f"{platform} attestation.started_at")
    completed_value = record_value(record, "completed_at", "stopped_at")
    if status == "completed":
        completed_at = parse_timestamp(completed_value, f"{platform} attestation.completed_at")
        if completed_at < started_at:
            fail(f"{platform} attestation completed_at 早于 started_at: {attestation_ref}")
        expected_hash = ensure_nonempty_string(receipt.get("output", {}).get("result_sha256"), f"{platform} receipt.output.result_sha256")
        actual_hash = ensure_nonempty_string(record.get("result_sha256"), f"{platform} attestation.result_sha256")
        if expected_hash != actual_hash:
            fail(f"{platform} receipt.result_sha256 与 attestation 不一致: {attestation_ref}")


def validate_runtime_invocation(receipt: dict[str, Any], repo_root: Path) -> None:
    executor = receipt.get("executor", {})
    if executor.get("platform") == "antigravity" and executor.get("kind") == "subagent":
        status = receipt.get("output", {}).get("status")
        if status not in {"running", "completed"}:
            return
        conversation_id = ensure_nonempty_string(executor.get("conversation_id"), "Antigravity receipt.executor.conversation_id")
        transcript_path = ensure_nonempty_string(executor.get("transcript_path"), "Antigravity receipt.executor.transcript_path")
        invocation_ref = ensure_nonempty_string(executor.get("invocation_ref"), "Antigravity receipt.executor.invocation_ref")
        events_path = repo_root / ".agent/state/.invocation-events.json"
        try:
            events = json.loads(events_path.read_text(encoding="utf-8"))
        except (FileNotFoundError, OSError, json.JSONDecodeError):
            fail(f"缺少 Antigravity invoke_subagent 审计记录: {invocation_ref}")
        if not isinstance(events, list):
            fail("Antigravity invoke_subagent 审计记录格式非法")
        event = next((item for item in events if isinstance(item, dict) and item.get("ref") == invocation_ref), None)
        if not isinstance(event, dict):
            fail(f"invocation_ref 未出现在 Antigravity 审计记录中: {invocation_ref}")
        if event.get("conversation_id") != conversation_id or event.get("transcript_path") != transcript_path:
            fail(f"invocation_ref 的会话或 transcript 证据不一致: {invocation_ref}")
        if status == "completed" and event.get("status") != "completed":
            fail(f"receipt 已 completed，但真实 invoke_subagent 尚未成功完成: {invocation_ref}")
        return
    validate_runtime_attestation(receipt, repo_root)


def completed_receipt(
    receipts: Iterable[dict[str, Any]],
    *,
    capability: str,
    stage: str | None,
    state_ref: str,
    fragment: str | None = None,
    platform_requirement: str | None = None,
) -> dict[str, Any] | None:
    matches: list[dict[str, Any]] = []
    for receipt in receipts:
        if receipt.get("capability") != capability or receipt.get("stage") != stage:
            continue
        if receipt.get("output", {}).get("status") != "completed":
            continue
        if fragment is not None and not ref_matches(receipt.get("output", {}).get("result_ref"), state_ref, fragment):
            continue
        if platform_requirement == "subagent" and (
            receipt.get("executor", {}).get("kind") != "subagent"
            or not receipt.get("executor", {}).get("invocation_ref")
        ):
            continue
        matches.append(receipt)
    if not matches:
        return None
    return matches[-1]


def latest_result(state: dict[str, Any], key: str, label: str) -> tuple[int, dict[str, Any]] | None:
    results = ensure_array(state.get(key), key)
    for index in range(len(results) - 1, -1, -1):
        if isinstance(results[index], dict):
            return index, results[index]
    return None


def latest_result_with(state: dict[str, Any], key: str, predicate: Any) -> tuple[int, dict[str, Any]] | None:
    results = ensure_array(state.get(key), key)
    for index in range(len(results) - 1, -1, -1):
        if isinstance(results[index], dict) and predicate(results[index]):
            return index, results[index]
    return None


def require_plan(state: dict[str, Any], receipts: list[dict[str, Any]], state_ref: str) -> None:
    plan = ensure_object(state.get("feature_plan"), "feature_plan")
    if plan.get("status") != "ready":
        fail("进入实现前必须存在 status=ready 的 Feature Plan")
    receipt = completed_receipt(receipts, capability="plan", stage="feature", state_ref=state_ref, fragment="feature_plan")
    if receipt is None:
        for candidate in reversed(receipts):
            if candidate.get("capability") != "plan" or candidate.get("stage") != "feature" or candidate.get("output", {}).get("status") != "completed":
                continue
            result_ref = candidate.get("output", {}).get("result_ref")
            if result_ref == plan.get("path") or (isinstance(result_ref, str) and result_ref.endswith(str(plan.get("path")))):
                receipt = candidate
                break
    if receipt is None:
        fail("缺少已完成的 plan receipt，或 receipt 未引用 feature_plan")


def require_workflow_receipt(
    receipts: list[dict[str, Any]],
    *,
    capability: str,
    stage: str,
    state_ref: str,
    require_completed: bool = False,
) -> dict[str, Any]:
    allowed_statuses = {"completed", "blocked"} if require_completed else {"running", "completed", "blocked"}
    matches = [
        receipt
        for receipt in receipts
        if receipt.get("capability") == capability
        and receipt.get("stage") == stage
        and receipt.get("output", {}).get("status") in allowed_statuses
        and receipt_ref_for_state(receipt, state_ref)
        and receipt.get("executor", {}).get("kind") == "workflow"
        and isinstance(receipt.get("executor", {}).get("invocation_ref"), str)
        and receipt.get("executor", {}).get("invocation_ref")
    ]
    if not matches:
        fail(f"缺少有效的 {capability} workflow receipt")
    return matches[-1]


def require_test_prepare(state: dict[str, Any], receipts: list[dict[str, Any]], state_ref: str) -> None:
    result = latest_result_with(state, "testing_results", lambda item: item.get("test_stage") == "prepare")
    if result is None:
        fail("缺少 Testing prepare 结果")
    index, item = result
    if item.get("stage_status") != "passed":
        fail("Testing prepare 未通过，不能进入实现")
    if completed_receipt(receipts, capability="test", stage="prepare", state_ref=state_ref, fragment=f"testing_results/{index}") is None:
        fail("缺少引用最新 Testing prepare 结果的 test receipt")


def require_test_verify(state: dict[str, Any], receipts: list[dict[str, Any]], state_ref: str) -> tuple[int, dict[str, Any]]:
    result = latest_result_with(state, "testing_results", lambda item: item.get("test_stage") == "verify")
    if result is None:
        fail("缺少 Testing verify 结果")
    index, item = result
    if item.get("stage_status") != "passed" or item.get("test_gate") not in {"passed", "not-evaluated"}:
        fail("最新 Testing verify 未形成可交接的稳定结果")
    if completed_receipt(receipts, capability="test", stage="verify", state_ref=state_ref, fragment=f"testing_results/{index}") is None:
        fail("缺少引用最新 Testing verify 结果的 test receipt")
    return result


def require_review(state: dict[str, Any], receipts: list[dict[str, Any]], state_ref: str) -> tuple[int, dict[str, Any]]:
    result = latest_result(state, "review_results", "review_results")
    if result is None:
        fail("缺少 Review 结果")
    index, item = result
    if item.get("status") != "REVIEW_PASSED":
        fail("最新 Review 不是 REVIEW_PASSED")
    if item.get("delegation_policy") not in {"required", "preferred", "unavailable"}:
        fail("Review 缺少有效 delegation_policy")
    if not isinstance(item.get("capability_run_ref"), str) or not item["capability_run_ref"]:
        fail("REVIEW_PASSED 缺少 capability_run_ref")
    if item.get("delegation_policy") == "required" and item.get("execution_mode") == "inline-fallback":
        fail("Review 在 delegation_policy=required 时不得报告 inline-fallback")
    if item.get("delegation_policy") == "required" and item.get("execution_mode") != "reviewer-agent":
        fail("Review 在 delegation_policy=required 时必须报告 reviewer-agent")
    dimensions = set(item.get("reviewed_dimensions", []))
    if dimensions != ALL_REVIEW_DIMENSIONS:
        fail("Review 未覆盖固定的七个维度")
    platform_requirement = "subagent" if any(
        receipt.get("executor", {}).get("platform") == "antigravity" and receipt.get("capability") == "review"
        for receipt in receipts
    ) else None
    if completed_receipt(
        receipts,
        capability="review",
        stage="feature",
        state_ref=state_ref,
        fragment=f"review_results/{index}",
        platform_requirement=platform_requirement,
    ) is None:
        fail("缺少引用最新 Review 结果的 review receipt，或 Antigravity Reviewer 未真实委派")
    target = item.get("review_target") or {}
    if target.get("base_ref") != state.get("implementation_baseline_ref"):
        fail("Review target.base_ref 与 implementation_baseline_ref 不一致")
    test_result = latest_result_with(state, "testing_results", lambda candidate: candidate.get("test_stage") == "verify")
    if test_result is not None:
        test_index, test_item = test_result
        if not ref_matches(item.get("testing_result_ref"), state_ref, f"testing_results/{test_index}"):
            fail("Review 的 testing_result_ref 未指向最新 Testing verify 结果")
        if not same_json(item.get("tested_change_fingerprint"), test_item.get("tested_change_fingerprint")):
            fail("Review 与最新 Testing 的产品快照不一致")
        if not same_json(item.get("tested_evidence_fingerprint"), test_item.get("tested_evidence_fingerprint")):
            fail("Review 与最新 Testing 的测试证据快照不一致")
        if not same_json(target.get("fingerprint"), test_item.get("tested_change_fingerprint")):
            fail("Review target.fingerprint 与最新 Testing 的产品快照不一致")
    if not same_json(item.get("reviewed_change_fingerprint"), target.get("fingerprint")):
        fail("Review reviewed_change_fingerprint 与 Review target 不一致")
    if not same_json(item.get("reviewed_evidence_fingerprint"), item.get("tested_evidence_fingerprint")):
        fail("Review reviewed_evidence_fingerprint 与输入测试证据不一致")
    if item.get("capability_run_ref") is not None:
        review_receipt = completed_receipt(receipts, capability="review", stage="feature", state_ref=state_ref, fragment=f"review_results/{index}", platform_requirement=platform_requirement)
        if review_receipt is None or item.get("capability_run_ref") != review_receipt.get("run_id"):
            fail("Review 的 capability_run_ref 必须严格指向本轮 Reviewer receipt.run_id")
    return result


def require_feature_verification(state: dict[str, Any], receipts: list[dict[str, Any]], state_ref: str) -> tuple[int, dict[str, Any]]:
    result = latest_result_with(
        state,
        "verification_results",
        lambda item: item.get("verification_level") == "feature",
    )
    if result is None:
        fail("缺少 Feature Verification 结果")
    index, item = result
    if item.get("status") != "VERIFICATION_PASSED":
        fail("最新 Feature Verification 未通过")
    review_result = latest_result(state, "review_results", "review_results")
    if review_result is not None:
        review_item = review_result[1]
        if not same_json(item.get("verified_change_fingerprint"), review_item.get("reviewed_change_fingerprint")):
            fail("Feature Verification 与 Review 的产品快照不一致")
    if completed_receipt(receipts, capability="verify", stage="feature", state_ref=state_ref, fragment=f"verification_results/{index}") is None:
        fail("缺少引用最新 Feature Verification 的 verify receipt")
    return result


def require_commit_preparation(state: dict[str, Any], receipts: list[dict[str, Any]], state_ref: str) -> None:
    preparation = ensure_object(state.get("commit_preparation"), "commit_preparation")
    for key in ("commit_message_run_ref", "verification_result_ref", "proposed_subject"):
        ensure_nonempty_string(preparation.get(key), f"commit_preparation.{key}")
    for key in ("scope_validated", "atomicity_validated"):
        if preparation.get(key) is not True:
            fail(f"commit_preparation.{key} 必须为 true")
    commit_message = completed_receipt(
        receipts,
        capability="commit-message",
        stage="feature",
        state_ref=state_ref,
        fragment="commit_preparation",
    )
    if commit_message is None:
        fail("缺少已完成的 commit-message receipt")
    if preparation.get("commit_message_run_ref") != commit_message.get("run_id"):
        fail("commit_preparation.commit_message_run_ref 未指向已完成的 commit-message receipt")
    verification = latest_result_with(
        state,
        "verification_results",
        lambda item: item.get("verification_level") == "feature",
    )
    if verification is None or not ref_matches(preparation.get("verification_result_ref"), state_ref, f"verification_results/{verification[0]}"):
        fail("commit_preparation.verification_result_ref 未指向最新 Feature Verification")
    verification_receipt = completed_receipt(
        receipts,
        capability="verify",
        stage="feature",
        state_ref=state_ref,
        fragment=f"verification_results/{verification[0]}",
    )
    if verification_receipt is None:
        fail("commit_preparation 缺少最新 Feature Verification receipt")
    if parse_timestamp(commit_message["started_at"], "commit-message receipt.started_at") < parse_timestamp(verification_receipt["completed_at"], "Feature Verification receipt.completed_at"):
        fail("commit-message receipt 发生在 Feature Verification 之前")


def require_prepared_commit(receipts: list[dict[str, Any]], state_ref: str) -> dict[str, Any]:
    matches = [
        receipt
        for receipt in receipts
        if receipt.get("capability") == "commit"
        and receipt.get("stage") == "feature"
        and receipt.get("output", {}).get("status") == "prepared"
        and receipt_ref_for_state(receipt, state_ref)
        and ref_matches(receipt.get("output", {}).get("result_ref"), state_ref, "commit")
    ]
    if not matches:
        fail("进入真实 git commit 前必须存在 prepared commit receipt")
    return matches[-1]


def require_commit(state: dict[str, Any], receipts: list[dict[str, Any]], state_ref: str) -> None:
    require_commit_preparation(state, receipts, state_ref)
    commit = ensure_object(state.get("commit"), "commit")
    if commit.get("status") != "committed":
        fail("Feature completed 前 commit.status 必须为 committed")
    sha = ensure_nonempty_string(commit.get("sha"), "commit.sha")
    if not SHA_RE.fullmatch(sha):
        fail("commit.sha 不是有效的 Git SHA")
    receipt = completed_receipt(receipts, capability="commit", stage="feature", state_ref=state_ref, fragment="commit")
    if receipt is None:
        fail("缺少已完成的 commit receipt")
    if commit.get("receipt_ref") != receipt.get("run_id"):
        fail("commit.receipt_ref 未指向已完成的 commit receipt")
    verification = completed_receipt(receipts, capability="verify", stage="feature", state_ref=state_ref, fragment="verification_results")
    if verification is not None:
        verify_time = parse_timestamp(verification["completed_at"], "Feature Verification receipt.completed_at")
        commit_time = parse_timestamp(receipt["started_at"], "commit receipt.started_at")
        if commit_time < verify_time:
            fail("commit receipt 发生在 Feature Verification 之前")


def validate_receipt_order(state: dict[str, Any], receipts: list[dict[str, Any]], state_ref: str, stage: str) -> None:
    sequence: list[tuple[str, dict[str, Any]]] = []
    plan = completed_receipt(receipts, capability="plan", stage="feature", state_ref=state_ref)
    if plan is not None and stage != "planning":
        sequence.append(("plan", plan))
    if state.get("test_mode") in {"required", "characterization"} and stage in {"implementing", "testing-verify", "reviewing", "verifying", "committing", "completed"}:
        prepare = completed_receipt(receipts, capability="test", stage="prepare", state_ref=state_ref)
        if prepare is not None:
            sequence.append(("test.prepare", prepare))
    if stage in {"reviewing", "verifying", "committing", "completed"}:
        verify = completed_receipt(receipts, capability="test", stage="verify", state_ref=state_ref)
        if verify is not None:
            sequence.append(("test.verify", verify))
    if stage in {"verifying", "committing", "completed"}:
        review = completed_receipt(receipts, capability="review", stage="feature", state_ref=state_ref)
        if review is not None:
            sequence.append(("review", review))
    if stage in {"committing", "completed"}:
        feature_verify = completed_receipt(receipts, capability="verify", stage="feature", state_ref=state_ref)
        if feature_verify is not None:
            sequence.append(("verify.feature", feature_verify))
    if stage == "completed":
        message = completed_receipt(receipts, capability="commit-message", stage="feature", state_ref=state_ref)
        commit = completed_receipt(receipts, capability="commit", stage="feature", state_ref=state_ref)
        if message is not None:
            sequence.append(("commit-message", message))
        if commit is not None:
            sequence.append(("commit", commit))
    previous_label: str | None = None
    previous_time: datetime | None = None
    for label, receipt in sequence:
        started = parse_timestamp(receipt["started_at"], f"{label} receipt.started_at")
        completed = parse_timestamp(receipt["completed_at"], f"{label} receipt.completed_at")
        if previous_time is not None and started < previous_time:
            fail(f"能力执行顺序非法: {label} 发生在 {previous_label} 完成之前")
        previous_label = label
        previous_time = completed


def validate_feature_progress(state: dict[str, Any], state_path: Path, target: str | None = None) -> None:
    if state.get("schema_version") != 6:
        fail("Feature state schema_version 必须为 6")
    current = state.get("current_stage")
    if current not in FEATURE_STAGES:
        fail(f"current_stage 非法: {current}")
    repo_root = repo_root_for(state_path)
    state_ref = state_ref_for(state_path, repo_root)
    receipts_by_capability = validate_receipts(state, repo_root)
    all_receipts = [receipt for receipts in receipts_by_capability.values() for receipt in receipts]
    for receipt in all_receipts:
        if not receipt_ref_for_state(receipt, state_ref):
            fail(f"receipt {receipt.get('run_id')} 的 input.state_ref 未指向当前 Feature state")

    stage = target or current
    if stage not in FEATURE_STAGES:
        fail(f"目标阶段非法: {stage}")
    if target is not None and current != target:
        allowed = {
            "planning": {"testing-prepare", "implementing", "blocked"},
            "testing-prepare": {"implementing", "blocked"},
            "implementing": {"testing-verify", "blocked"},
            "testing-verify": {"reviewing", "blocked"},
            "reviewing": {"verifying", "blocked"},
            "verifying": {"committing", "blocked"},
            "committing": {"completed", "blocked"},
            "blocked": {"planning", "testing-prepare", "implementing", "testing-verify", "reviewing", "verifying", "committing"},
        }
        if target not in allowed.get(current, set()):
            fail(f"不允许的 Feature 状态转换: {current} -> {target}")

    require_workflow_receipt(
        all_receipts,
        capability="feature-development",
        stage="feature",
        state_ref=state_ref,
        require_completed=stage == "completed",
    )

    if stage not in {"planning", "blocked"}:
        require_plan(state, all_receipts, state_ref)
    if stage in {"implementing", "testing-verify", "reviewing", "verifying", "committing", "completed"}:
        if state.get("test_mode") in {"required", "characterization"}:
            require_test_prepare(state, all_receipts, state_ref)
    if stage in {"reviewing", "verifying", "committing", "completed"}:
        require_test_verify(state, all_receipts, state_ref)
    if stage in {"verifying", "committing", "completed"}:
        require_review(state, all_receipts, state_ref)
    if stage in {"committing", "completed"}:
        require_feature_verification(state, all_receipts, state_ref)
    if stage == "committing":
        require_commit_preparation(state, all_receipts, state_ref)
        prepared_commit = require_prepared_commit(all_receipts, state_ref)
        commit_message = completed_receipt(all_receipts, capability="commit-message", stage="feature", state_ref=state_ref, fragment="commit_preparation")
        if commit_message is None:
            fail("prepared commit 缺少已完成的 commit-message receipt")
        if parse_timestamp(prepared_commit["started_at"], "prepared commit receipt.started_at") < parse_timestamp(commit_message["completed_at"], "commit-message receipt.completed_at"):
            fail("prepared commit receipt 发生在 commit-message receipt 完成之前")
    if stage == "completed":
        require_commit(state, all_receipts, state_ref)
    validate_receipt_order(state, all_receipts, state_ref, stage)


def resolve_state_path(raw: str) -> Path:
    path = Path(raw)
    if not path.is_absolute():
        path = Path.cwd() / path
    return path.resolve()


def validate_phase(state_path: Path) -> None:
    state = ensure_object(load_json(state_path), "Phase state")
    if state.get("schema_version") != 3:
        fail("Phase state schema_version 必须为 3")
    status = state.get("status")
    if status not in {"running", "verifying", "completed", "blocked"}:
        fail(f"Phase status 非法: {status}")
    repo_root = repo_root_for(state_path)
    receipts = validate_receipts(state, repo_root)
    all_receipts = [receipt for values in receipts.values() for receipt in values]
    phase_ref = state_ref_for(state_path, repo_root)
    require_workflow_receipt(
        all_receipts,
        capability="phase-development",
        stage="phase",
        state_ref=phase_ref,
        require_completed=status == "completed",
    )
    for receipt in all_receipts:
        if not receipt_ref_for_state(receipt, phase_ref):
            fail(f"receipt {receipt.get('run_id')} 的 input.state_ref 未指向当前 Phase state")
    feature_runs = ensure_array(state.get("feature_runs"), "feature_runs")
    if not feature_runs:
        fail("Phase 必须至少登记一个 Feature")
    ids = {item.get("feature_id") for item in feature_runs if isinstance(item, dict)}
    if len(ids) != len(feature_runs) or None in ids:
        fail("Phase feature_runs 必须有唯一 feature_id")
    completed_ids = {item.get("feature_id") for item in feature_runs if item.get("status") == "completed"}
    for item in feature_runs:
        dependencies = item.get("dependencies")
        if not isinstance(dependencies, list) or any(dependency not in ids for dependency in dependencies):
            fail(f"Feature 依赖引用不存在: {item.get('feature_id')}")
        if item.get("status") in {"active", "completed"} and any(dependency not in completed_ids for dependency in dependencies):
            fail(f"Feature 尚未满足依赖却已执行或完成: {item.get('feature_id')}")
    active = [item for item in feature_runs if item.get("status") == "active"]
    if len(active) > 1:
        fail("Phase 同时存在多个 active Feature")
    if active and state.get("current_feature_id") != active[0].get("feature_id"):
        fail("current_feature_id 未指向唯一 active Feature")
    if not active and status == "running" and state.get("current_feature_id") is not None:
        fail("running Phase 没有 active Feature，但 current_feature_id 非空")

    baseline_sha = git_commit_sha(repo_root, state.get("baseline_ref"), "Phase baseline_ref")
    head_sha = git_commit_sha(repo_root, "HEAD", "当前 HEAD")
    baseline_ancestor = subprocess.run(
        ["git", "merge-base", "--is-ancestor", baseline_sha, head_sha],
        cwd=repo_root,
        check=False,
        capture_output=True,
    )
    if baseline_ancestor.returncode != 0:
        fail("Phase baseline_ref 不是当前 HEAD 的祖先")

    all_completed = True
    expected_commit_shas: set[str] = set()
    commit_owners: dict[str, str] = {}
    for run in feature_runs:
        item = ensure_object(run, "feature_runs item")
        state_ref = item.get("state_ref")
        if item.get("status") == "completed":
            if not isinstance(state_ref, str) or not state_ref:
                fail(f"已完成 Feature 缺少 state_ref: {item.get('feature_id')}")
            commit_shas = item.get("commit_shas")
            if not isinstance(commit_shas, list) or not commit_shas or any(not isinstance(sha, str) or not SHA_RE.fullmatch(sha) for sha in commit_shas):
                fail(f"已完成 Feature 的 commit_shas 缺少真实 SHA: {item.get('feature_id')}")
            for sha in commit_shas:
                previous_owner = commit_owners.get(sha.lower())
                if previous_owner is not None and previous_owner != item.get("feature_id"):
                    fail(f"同一个 Git commit SHA 被多个 Feature 记录: {sha}")
                commit_owners[sha.lower()] = item.get("feature_id")
                expected_commit_shas.add(git_commit_sha(repo_root, sha, f"Feature {item.get('feature_id')} commit SHA"))
            child_path = (repo_root / state_ref).resolve() if not Path(state_ref).is_absolute() else Path(state_ref)
            if not child_path.is_file():
                fail(f"Feature state 不存在: {state_ref}")
            child = ensure_object(load_json(child_path), f"Feature state {state_ref}")
            if child.get("feature_id") != item.get("feature_id"):
                fail(f"Feature state 身份与 Phase feature_runs 不一致: {item.get('feature_id')}")
            if child.get("phase_id") != state.get("phase_id"):
                fail(f"Feature state 未归属当前 Phase: {item.get('feature_id')}")
            validate_feature_progress(child, child_path)
            if child.get("current_stage") != "completed":
                fail(f"Phase 标记完成的 Feature 尚未 completed: {item.get('feature_id')}")
            child_sha = child.get("commit", {}).get("sha")
            if not child_sha or child_sha not in item.get("commit_shas", []):
                fail(f"Phase 未引用 Feature 的真实 commit SHA: {item.get('feature_id')}")
        else:
            all_completed = False

    if status == "completed":
        if not all_completed:
            fail("PHASE_COMPLETED 前必须完成全部 Feature")
        if state.get("current_feature_id") is not None:
            fail("completed Phase 的 current_feature_id 必须为 null")
        if not isinstance(state.get("final_ref"), str) or not state["final_ref"]:
            fail("completed Phase 缺少 final_ref")
        final_sha = git_commit_sha(repo_root, state["final_ref"], "Phase final_ref")
        if final_sha != head_sha:
            fail("Phase final_ref 未指向当前 HEAD")
        phase_result = latest_result_with(
            state,
            "verification_results",
            lambda item: item.get("verification_level") == "phase",
        )
        if phase_result is None or phase_result[1].get("status") != "VERIFICATION_PASSED":
            fail("completed Phase 缺少通过的 Phase Verification")
        phase_index = phase_result[0]
        if completed_receipt(all_receipts, capability="verify", stage="phase", state_ref=phase_ref, fragment=f"verification_results/{phase_index}") is None:
            fail("completed Phase 缺少引用 Phase Verification 的 verify receipt")

        actual_commit_shas = {
            line
            for line in git_output(repo_root, "rev-list", f"{baseline_sha}..{head_sha}").splitlines()
            if line
        }
        if actual_commit_shas != expected_commit_shas:
            missing = sorted(expected_commit_shas - actual_commit_shas)
            unexpected = sorted(actual_commit_shas - expected_commit_shas)
            fail(f"Phase Git 提交集合与 Feature commit_shas 不一致: missing={missing}, unexpected={unexpected}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate Harness orchestration semantics")
    subparsers = parser.add_subparsers(dest="command", required=True)

    feature_parser = subparsers.add_parser("feature")
    feature_parser.add_argument("state")

    transition_parser = subparsers.add_parser("transition")
    transition_parser.add_argument("state")
    transition_parser.add_argument("--to", required=True, dest="target")

    phase_parser = subparsers.add_parser("phase")
    phase_parser.add_argument("state")

    args = parser.parse_args()
    try:
        state_path = resolve_state_path(args.state)
        if args.command == "phase":
            validate_phase(state_path)
            print(f"ORCHESTRATION_VALID phase {state_path}")
        else:
            state = ensure_object(load_json(state_path), "Feature state")
            validate_feature_progress(state, state_path, getattr(args, "target", None))
            if args.command == "transition":
                print(f"ORCHESTRATION_VALID transition {state.get('current_stage')} -> {args.target}")
            else:
                print(f"ORCHESTRATION_VALID feature {state_path}")
        return 0
    except ValidationError as exc:
        print(f"ORCHESTRATION_INVALID: {exc}", file=sys.stderr)
        return 1
    except OSError as exc:
        print(f"ORCHESTRATION_INVALID: 无法读取状态: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
