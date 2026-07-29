"""
tests/test_paddle_perf.py
--------------------------
Unit tests for Paddle Native Performance evaluation (scripts/parity/check_paddle_perf.py).
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

from scripts.parity.check_paddle_perf import (
    PaddlePerfMetrics,
    evaluate_paddle_perf_hard_gates,
    generate_markdown_perf_report,
    run_paddle_perf_check,
)


def test_evaluate_paddle_perf_hard_gates_pass() -> None:
    oracle = PaddlePerfMetrics(wall_time_sec=10.0, peak_rss_mb=200.0)
    candidate = PaddlePerfMetrics(
        wall_time_sec=8.0, peak_rss_mb=150.0, memory_leaks_count=0
    )
    report = evaluate_paddle_perf_hard_gates(oracle, candidate, quality_passed=True)
    assert report.overall_passed is True
    assert len(report.gate_results) == 4
    assert all(g.passed for g in report.gate_results)


def test_evaluate_paddle_perf_hard_gates_wall_time_breach() -> None:
    oracle = PaddlePerfMetrics(wall_time_sec=10.0, peak_rss_mb=200.0)
    # Candidate Wall-time is 13.0s (1.30x > 1.20x limit)
    candidate = PaddlePerfMetrics(
        wall_time_sec=13.0, peak_rss_mb=150.0, memory_leaks_count=0
    )
    report = evaluate_paddle_perf_hard_gates(oracle, candidate, quality_passed=True)
    assert report.overall_passed is False
    wall_gate = next(g for g in report.gate_results if g.name == "wall_time_ratio")
    assert wall_gate.passed is False
    assert "FAIL" in wall_gate.message


def test_evaluate_paddle_perf_hard_gates_rss_breach() -> None:
    oracle = PaddlePerfMetrics(wall_time_sec=10.0, peak_rss_mb=200.0)
    # Candidate RSS is 190.0MB (0.95x > 0.90x limit)
    candidate = PaddlePerfMetrics(
        wall_time_sec=8.0, peak_rss_mb=190.0, memory_leaks_count=0
    )
    report = evaluate_paddle_perf_hard_gates(oracle, candidate, quality_passed=True)
    assert report.overall_passed is False
    rss_gate = next(g for g in report.gate_results if g.name == "peak_rss_ratio")
    assert rss_gate.passed is False


def test_evaluate_paddle_perf_hard_gates_memory_leak_breach() -> None:
    oracle = PaddlePerfMetrics(wall_time_sec=10.0, peak_rss_mb=200.0)
    candidate = PaddlePerfMetrics(
        wall_time_sec=8.0, peak_rss_mb=150.0, memory_leaks_count=2
    )
    report = evaluate_paddle_perf_hard_gates(oracle, candidate, quality_passed=True)
    assert report.overall_passed is False
    leak_gate = next(g for g in report.gate_results if g.name == "zero_memory_leaks")
    assert leak_gate.passed is False


def test_evaluate_paddle_perf_hard_gates_quality_breach() -> None:
    oracle = PaddlePerfMetrics(wall_time_sec=10.0, peak_rss_mb=200.0)
    candidate = PaddlePerfMetrics(wall_time_sec=8.0, peak_rss_mb=150.0)
    report = evaluate_paddle_perf_hard_gates(
        oracle, candidate, quality_passed=False
    )
    assert report.overall_passed is False


def test_generate_markdown_perf_report() -> None:
    oracle = PaddlePerfMetrics(wall_time_sec=10.0, peak_rss_mb=200.0)
    candidate = PaddlePerfMetrics(wall_time_sec=8.0, peak_rss_mb=150.0)
    report = evaluate_paddle_perf_hard_gates(oracle, candidate, quality_passed=True)

    md = generate_markdown_perf_report(report)
    assert (
        "# SubLift Phase 6.8 — Paddle Native Performance Hardening Report" in md
    )
    assert "PASS ✅" in md
    assert "wall_time_ratio" in md


def test_run_paddle_perf_check_script_execution(tmp_path: Path) -> None:
    report_file = tmp_path / "paddle_perf.md"
    json_file = tmp_path / "paddle_perf.json"

    report = run_paddle_perf_check(
        check=True,
        report_out=report_file,
        json_out=json_file,
        skip_runtime=True,
    )
    assert report.overall_passed is True
    assert report_file.exists()
    assert json_file.exists()


def test_cli_paddle_perf_execution(tmp_path: Path) -> None:
    report_file = tmp_path / "cli_paddle_perf.md"
    json_file = tmp_path / "cli_paddle_perf.json"
    cmd = [
        sys.executable,
        "scripts/parity/check_paddle_perf.py",
        "--check",
        "--skip-runtime",
        "--report-out",
        str(report_file),
        "--json-out",
        str(json_file),
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    assert proc.returncode == 1
    assert "C++ Paddle runtime is skipped or unavailable" in proc.stderr
    assert report_file.exists()
