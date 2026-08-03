from __future__ import annotations

import hashlib
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path


VALIDATOR_PATH = Path(__file__).parents[1] / "scripts/validate-orchestration.py"
SPEC = importlib.util.spec_from_file_location("validate_orchestration", VALIDATOR_PATH)
assert SPEC and SPEC.loader
validate_orchestration = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(validate_orchestration)


class RuntimeAttestationTests(unittest.TestCase):
    def make_receipt(
        self,
        root: Path,
        *,
        platform: str = "opencode",
        category: str = "tasks",
        name: str = "task-1",
        agent: str = "planner",
        status: str = "completed",
        conversation: str | None = "child-1",
        result_hash: str | None = None,
        transcript: str | None = None,
    ) -> dict:
        state_ref = ".agent/state/features/F-test.json"
        attestation_ref = f".agent/state/runtime/{platform}/{category}/{name}.json"
        result_hash = result_hash or hashlib.sha256(b"result").hexdigest()
        record = {
            "schema_version": 1,
            "platform": platform,
            "kind": "subagent" if category == "tasks" or category == "subagents" else "workflow",
            "agent_name": agent if platform == "opencode" else None,
            "agent_type": agent if platform == "claude-code" else None,
            "agent_id": conversation if platform == "claude-code" else None,
            "session_id": "parent-1" if platform == "claude-code" else None,
            "parent_session_id": "parent-1",
            "conversation_id": conversation,
            "child_session_id": conversation if platform == "opencode" else None,
            "call_id": name,
            "state_ref": state_ref,
            "transcript_path": transcript,
            "status": status,
            "started_at": "2026-08-02T00:00:00Z",
            "completed_at": "2026-08-02T00:01:00Z" if status == "completed" else None,
            "result_sha256": result_hash if status == "completed" else None,
        }
        path = root / attestation_ref
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(record), encoding="utf-8")
        return {
            "schema_version": 3,
            "run_id": f"run-{name}",
            "capability": "plan",
            "stage": "feature",
            "entrypoint": ".agent/skills/plan/SKILL.md",
            "delegation_policy": "required",
            "executor": {
                "kind": record["kind"],
                "platform": platform,
                "agent_name": agent,
                "invocation_ref": attestation_ref,
                "attestation_ref": attestation_ref,
                "conversation_id": conversation,
                "parent_session_id": "parent-1",
                "call_id": name,
                "transcript_path": transcript,
            },
            "input": {"state_ref": state_ref, "fingerprint": None},
            "output": {
                "result_ref": f"{state_ref}#/feature_plan",
                "status": status,
                "result_sha256": result_hash if status == "completed" else None,
            },
            "started_at": "2026-08-02T00:00:00Z",
            "completed_at": "2026-08-02T00:01:00Z" if status == "completed" else None,
        }

    def test_opencode_completed_attestation_matches_call_and_result(self) -> None:
        with tempfile.TemporaryDirectory(prefix="harness-runtime-test-") as temporary:
            root = Path(temporary)
            receipt = self.make_receipt(root)
            validate_orchestration.validate_runtime_attestation(receipt, root)

    def test_claude_completed_attestation_requires_existing_transcript(self) -> None:
        with tempfile.TemporaryDirectory(prefix="harness-runtime-test-") as temporary:
            root = Path(temporary)
            transcript = "transcript.jsonl"
            receipt = self.make_receipt(root, platform="claude-code", category="subagents", name="agent-1", transcript=transcript)
            with self.assertRaises(validate_orchestration.ValidationError):
                validate_orchestration.validate_runtime_attestation(receipt, root)
            (root / transcript).write_text("{}\n", encoding="utf-8")
            validate_orchestration.validate_runtime_attestation(receipt, root)

    def test_completed_receipt_rejects_result_hash_mismatch(self) -> None:
        with tempfile.TemporaryDirectory(prefix="harness-runtime-test-") as temporary:
            root = Path(temporary)
            receipt = self.make_receipt(root)
            receipt["output"]["result_sha256"] = "f" * 64
            with self.assertRaises(validate_orchestration.ValidationError):
                validate_orchestration.validate_runtime_attestation(receipt, root)

    def test_running_receipt_can_wait_for_child_conversation(self) -> None:
        with tempfile.TemporaryDirectory(prefix="harness-runtime-test-") as temporary:
            root = Path(temporary)
            receipt = self.make_receipt(root, status="running", conversation=None)
            validate_orchestration.validate_runtime_attestation(receipt, root)

    def test_attestation_ref_cannot_escape_runtime_directory(self) -> None:
        with tempfile.TemporaryDirectory(prefix="harness-runtime-test-") as temporary:
            root = Path(temporary)
            receipt = self.make_receipt(root)
            receipt["executor"]["attestation_ref"] = ".agent/state/features/F-test.json"
            with self.assertRaises(validate_orchestration.ValidationError):
                validate_orchestration.validate_runtime_attestation(receipt, root)

    def test_capability_receipt_requires_v3_attestation_fields(self) -> None:
        with tempfile.TemporaryDirectory(prefix="harness-runtime-test-") as temporary:
            root = Path(temporary)
            receipt = self.make_receipt(root)
            validate_orchestration.validate_receipt(receipt, 0, set())
            del receipt["executor"]["attestation_ref"]
            with self.assertRaises(validate_orchestration.ValidationError):
                validate_orchestration.validate_receipt(receipt, 0, set())


if __name__ == "__main__":
    unittest.main()
