"""
tests/test_paddle_gate.py
--------------------------
Unit tests for Paddle E2E quality gate evaluation (scripts/parity/check_paddle_gate.py).
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

from scripts.parity.check_paddle_gate import (
    PaddleGateMetrics,
    compute_box_iou,
    evaluate_paddle_hard_gates,
    generate_markdown_report,
    run_paddle_gate,
)

from sublift.models import BoundingBox


def test_compute_box_iou() -> None:
    box1 = BoundingBox(x=10, y=10, width=100, height=50)
    box2 = BoundingBox(x=10, y=10, width=100, height=50)
    assert compute_box_iou(box1, box2) == 1.0

    box3 = BoundingBox(x=200, y=200, width=50, height=50)
    assert compute_box_iou(box1, box3) == 0.0

    box4 = BoundingBox(x=60, y=10, width=100, height=50)
    # intersection: x=[60..110] -> width 50, y=[10..60] -> height 50, area 2500
    # union: 5000 + 5000 - 2500 = 7500. iou = 2500/7500 = 1/3
    assert abs(compute_box_iou(box1, box4) - (1.0 / 3.0)) < 1e-4


def test_evaluate_paddle_hard_gates_pass() -> None:
    oracle = PaddleGateMetrics(
        timing_f1=0.95,
        timing_precision=0.96,
        cer_macro=0.05,
        usable_recall=0.90,
        line_box_mean_iou=0.95,
    )
    candidate = PaddleGateMetrics(
        timing_f1=0.945,
        timing_precision=0.955,
        cer_macro=0.055,
        usable_recall=0.89,
        line_box_mean_iou=0.94,
    )
    report = evaluate_paddle_hard_gates(oracle, candidate)
    assert report.overall_passed is True
    assert len(report.gate_results) == 6
    assert all(g.passed for g in report.gate_results)


def test_evaluate_paddle_hard_gates_cer_micro_breach() -> None:
    oracle = PaddleGateMetrics(cer_micro=0.03)
    candidate = PaddleGateMetrics(cer_micro=0.05)  # increase by 0.02 > 0.010
    report = evaluate_paddle_hard_gates(oracle, candidate)
    assert report.overall_passed is False
    micro_gate = next(g for g in report.gate_results if g.name == "cer_micro_relative")
    assert micro_gate.passed is False


def test_evaluate_paddle_hard_gates_breach() -> None:
    oracle = PaddleGateMetrics(
        timing_f1=0.95,
        timing_precision=0.96,
        cer_macro=0.05,
        usable_recall=0.90,
        line_box_mean_iou=0.95,
    )
    # Candidate F1 drops by 0.02 (2.0% > 1.0% allowed limit)
    candidate = PaddleGateMetrics(
        timing_f1=0.93,
        timing_precision=0.96,
        cer_macro=0.05,
        usable_recall=0.90,
        line_box_mean_iou=0.95,
    )
    report = evaluate_paddle_hard_gates(oracle, candidate)
    assert report.overall_passed is False
    f1_gate = next(g for g in report.gate_results if g.name == "timing_f1_relative")
    assert f1_gate.passed is False
    assert "FAIL" in f1_gate.message


def test_generate_markdown_report() -> None:
    oracle = PaddleGateMetrics(timing_f1=0.95)
    candidate = PaddleGateMetrics(timing_f1=0.95)
    report = evaluate_paddle_hard_gates(oracle, candidate)

    md = generate_markdown_report(report)
    assert "# SubLift Phase 6.8 — Paddle E2E Quality Gate Report" in md
    assert "PASS ✅" in md
    assert "timing_f1_relative" in md


def test_run_paddle_gate_script_execution(tmp_path: Path) -> None:
    report_file = tmp_path / "paddle_gate.md"
    json_file = tmp_path / "paddle_gate.json"

    report = run_paddle_gate(
        check=True,
        report_out=report_file,
        json_out=json_file,
        skip_runtime=True,
    )
    assert report.overall_passed is False
    assert report_file.exists()
    assert json_file.exists()


def test_cli_paddle_gate_execution(tmp_path: Path) -> None:
    report_file = tmp_path / "cli_paddle_gate.md"
    json_file = tmp_path / "cli_paddle_gate.json"
    cmd = [
        sys.executable,
        "scripts/parity/check_paddle_gate.py",
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
