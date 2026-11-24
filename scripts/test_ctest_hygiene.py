#!/usr/bin/env python3
"""
Static test hygiene & layering governance checks for SubLift.

Enforces that:
1. Every CTest target registered in CMakeLists.txt / cpp/tests/CMakeLists.txt
   declares valid LABELS within {"unit", "parity", "adapter", "e2e"}.
2. No forbidden sprawling test executable/test names (smoke_*, *_adversarial*, *_stub*)
   are introduced, and test executables remain consolidated (<= 25).
3. C++ unit/parity/adapter tests do not reintroduce brittle wall-clock elapsed < N assertions.
4. Centralized E2E gate config benchmark/gates.json stays valid and structured.
"""

from __future__ import annotations

import json
from pathlib import Path
import re
import unittest

PROJECT_ROOT = Path(__file__).resolve().parent.parent
VALID_LABELS = {"unit", "parity", "adapter", "e2e"}
FORBIDDEN_TEST_NAME_PATTERNS = (
    re.compile(r"^smoke_"),
    re.compile(r"_adversarial"),
    re.compile(r"_stub"),
)
ELAPSED_ASSERT_PATTERN = re.compile(
    r"\b(?:REQUIRE|CHECK)\s*\(\s*elapsed(?:_ms|_s|_sec|_us)?\s*<"
)


def _strip_cmake_comments(text: str) -> str:
    return "\n".join(line.split("#", 1)[0] for line in text.splitlines())


def _strip_cpp_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    return "\n".join(line.split("//", 1)[0] for line in text.splitlines())


class TestCTestHygiene(unittest.TestCase):
    def test_all_ctest_targets_have_valid_labels_and_no_forbidden_names(self) -> None:
        cmake_files = [
            PROJECT_ROOT / "CMakeLists.txt",
            PROJECT_ROOT / "cpp" / "tests" / "CMakeLists.txt",
        ]
        registered_tests: list[str] = []
        test_labels: dict[str, set[str]] = {}

        for cmake_file in cmake_files:
            content = _strip_cmake_comments(cmake_file.read_text(encoding="utf-8"))
            for m in re.finditer(r"\badd_test\s*\(\s*NAME\s+([A-Za-z0-9_.-]+)", content):
                registered_tests.append(m.group(1))

            for m in re.finditer(r"\bset_tests_properties\s*\(([^)]+)\)", content, flags=re.DOTALL):
                body = m.group(1)
                parts = re.split(r"\bPROPERTIES\b", body, maxsplit=1)
                if len(parts) != 2:
                    continue
                names_part, props_part = parts
                names = [tok for tok in names_part.split() if tok]
                lbl_match = re.search(r'\bLABELS\s+(?:"([^"]+)"|([^\s)]+))', props_part)
                if not lbl_match:
                    continue
                raw_labels = lbl_match.group(1) or lbl_match.group(2) or ""
                labels = {lbl.strip() for lbl in raw_labels.split(";") if lbl.strip()}
                for name in names:
                    test_labels.setdefault(name, set()).update(labels)

        self.assertGreater(len(registered_tests), 0, "No CTest targets discovered")

        for test_name in registered_tests:
            for pat in FORBIDDEN_TEST_NAME_PATTERNS:
                self.assertIsNone(
                    pat.search(test_name),
                    f"Forbidden sprawling CTest target name: {test_name}",
                )
            labels = test_labels.get(test_name, set())
            self.assertTrue(
                labels,
                f"CTest target '{test_name}' must declare LABELS (unit/parity/adapter/e2e)",
            )
            invalid = labels - VALID_LABELS
            self.assertFalse(
                invalid,
                f"CTest target '{test_name}' has invalid LABELS: {sorted(invalid)}",
            )

    def test_cpp_test_executables_remain_consolidated(self) -> None:
        tests_cmake = _strip_cmake_comments(
            (PROJECT_ROOT / "cpp" / "tests" / "CMakeLists.txt").read_text(encoding="utf-8")
        )
        executables = re.findall(r"\badd_executable\s*\(\s*([A-Za-z0-9_.-]+)", tests_cmake)
        self.assertLessEqual(
            len(executables),
            25,
            f"Too many test executables ({len(executables)} > 25): {executables}",
        )

    def test_no_hardcoded_elapsed_wall_clock_assertions(self) -> None:
        tests_dir = PROJECT_ROOT / "cpp" / "tests"
        offenders: list[str] = []
        for cpp_file in sorted(tests_dir.rglob("*.cpp")):
            cleaned = _strip_cpp_comments(cpp_file.read_text(encoding="utf-8"))
            for lineno, line in enumerate(cleaned.splitlines(), 1):
                if ELAPSED_ASSERT_PATTERN.search(line):
                    offenders.append(f"{cpp_file.relative_to(PROJECT_ROOT)}:{lineno}: {line.strip()}")

        self.assertEqual(
            offenders,
            [],
            "Hardcoded wall-clock elapsed assertions found in cpp/tests:\n" + "\n".join(offenders),
        )

    def test_centralized_gates_json_schema(self) -> None:
        gates_path = PROJECT_ROOT / "benchmark" / "gates.json"
        self.assertTrue(gates_path.exists(), "benchmark/gates.json must exist")
        data = json.loads(gates_path.read_text(encoding="utf-8"))
        for key in ("dataset", "tracks", "e2e_smoke_clip"):
            self.assertIn(key, data, f"Missing required key '{key}' in benchmark/gates.json")


if __name__ == "__main__":
    unittest.main()
