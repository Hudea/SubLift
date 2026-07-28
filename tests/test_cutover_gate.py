"""Cutover Gate 校验脚本 (check_cutover_gate.py) 单元/集成测试。"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path
from unittest.mock import patch

from scripts.parity.check_cutover_gate import (
    CANCEL_MAX_S,
    RESTART_MAX_S,
    CutoverGateReport,
    GtL3MetricRow,
    GtL3Result,
    ParityCheckResult,
    RuntimeMetrics,
    generate_markdown_report,
    run_correctness_checks,
    run_cutover_gate,
    run_gt_l3_check,
)
from scripts.parity.golden_registry import PARITY_SCRIPTS


def test_correctness_checks_execution() -> None:
    results = run_correctness_checks()
    assert len(results) == 10
    names = [r.name for r in results]
    assert "config" in names
    assert "signature" in names
    assert "changepoint" in names
    assert "timeline" in names
    assert "dedupe" in names
    assert "line_select" in names
    assert "pipeline" in names
    assert "extractor" in names
    assert "vision" in names
    assert "ipc_session" in names
    assert all(r.passed for r in results)


def test_generate_markdown_report_formatting() -> None:
    report = CutoverGateReport(
        parity_results=[
            ParityCheckResult(
                name="test_mod",
                script_path=Path("dump_test.py"),
                passed=True,
                duration_s=0.123,
                message="PASS\nline2",
            )
        ],
        python_metrics=RuntimeMetrics("python", 1.0, 0.05, 0.10, 50.0),
        cpp_metrics=RuntimeMetrics("cpp", 0.8, 0.02, 0.08, 30.0),
        gt_result=GtL3Result(
            status="waived",
            passed=True,
            message="Fixed GT video absent; ADR-0022",
        ),
        correctness_passed=True,
        runtime_passed=True,
        gt_passed=True,
        all_passed=True,
        gates_run=["correctness_parity", "runtime_wall_cancel_restart_rss", "gt_l3_waived"],
        gates_missing=["gt_l3_live_measurement"],
    )

    md = generate_markdown_report(report)
    assert "# Phase 6.6 Cutover Gate 验证报告" in md
    assert "✅ PASS" in md
    assert "test_mod" in md
    assert "PASS line2" in md
    assert "Wall-time Duration" in md
    assert "Cancel Latency" in md
    assert "Restart Latency" in md
    assert "Peak RSS Memory" in md
    assert "GT L3" in md
    assert "WAIVED" in md
    assert "已执行门禁" in md
    # Must not claim full publish-contract when GT waived
    assert "非完整发布契约" in md or "未执行/豁免" in md


def test_markdown_report_failed_runtime_threshold() -> None:
    report = CutoverGateReport(
        parity_results=[],
        python_metrics=RuntimeMetrics("python", 1.0, 0.05, 0.1, 50.0),
        cpp_metrics=RuntimeMetrics("cpp", 10.0, 0.02, 0.08, 30.0),
        gt_result=None,
        correctness_passed=True,
        runtime_passed=False,
        gt_passed=True,
        all_passed=False,
    )

    md = generate_markdown_report(report)
    assert "❌ FAIL" in md
    assert "❌ FAIL (C++ 耗时超出预值)" in md


def test_markdown_report_restart_threshold() -> None:
    report = CutoverGateReport(
        parity_results=[],
        python_metrics=RuntimeMetrics("python", 1.0, 0.05, 0.1, 50.0),
        cpp_metrics=RuntimeMetrics("cpp", 0.8, 0.02, 6.0, 30.0),
        gt_result=None,
        correctness_passed=True,
        runtime_passed=False,
        gt_passed=True,
        all_passed=False,
    )
    md = generate_markdown_report(report)
    assert "Restart Latency" in md
    assert "❌ FAIL (>5.0s)" in md


def test_runtime_thresholds_constants() -> None:
    assert CANCEL_MAX_S == 1.0
    assert RESTART_MAX_S == 5.0


def test_run_cutover_gate_skip_runtime(tmp_path: Path) -> None:
    report_file = tmp_path / "gate_report.md"
    report = run_cutover_gate(
        check=True,
        skip_runtime=True,
        skip_gt=True,
        report_out=report_file,
    )
    assert report.correctness_passed is True
    assert report.runtime_skipped is True
    assert report.all_passed is True
    assert report_file.exists()
    assert "# Phase 6.6 Cutover Gate 验证报告" in report_file.read_text(encoding="utf-8")


def test_golden_registry_matches_correctness_suite() -> None:
    assert len(PARITY_SCRIPTS) == 10
    results = run_correctness_checks()
    assert [r.name for r in results] == [n for n, _ in PARITY_SCRIPTS]


def test_run_cutover_gate_parity_only() -> None:
    report = run_cutover_gate(check=True, parity_only=True)
    assert report.correctness_passed is True
    assert len(report.parity_results) == 10
    assert report.runtime_skipped is True
    assert report.gt_result is None
    assert report.all_passed is True


def test_missing_worker_fails_runtime_without_skip(tmp_path: Path) -> None:
    """GATE-05: missing sublift_worker must fail when not --skip-runtime."""
    report_file = tmp_path / "gate_missing_worker.md"
    missing = tmp_path / "no_such_worker"
    with patch(
        "scripts.parity.check_cutover_gate.resolve_worker_bin",
        return_value=None,
    ), patch(
        "scripts.parity.check_cutover_gate.worker_bin",
        return_value=missing,
    ):
        report = run_cutover_gate(
            check=True,
            skip_runtime=False,
            skip_gt=True,
            report_out=report_file,
        )
    assert report.runtime_passed is False
    assert report.all_passed is False


def test_gt_l3_waived_when_video_missing() -> None:
    result = run_gt_l3_check(skip_gt=False, require_gt=False)
    # Video is not vendored in the repo on typical dev/CI machines
    if not (Path("debug") / "Zootopia_clip_1080p.mp4").exists():
        assert result.status == "waived"
        assert result.passed is True
        assert "ADR-0022" in result.message


def test_gt_l3_require_fails_without_video() -> None:
    result = run_gt_l3_check(skip_gt=False, require_gt=True)
    if not (Path("debug") / "Zootopia_clip_1080p.mp4").exists():
        assert result.status == "failed"
        assert result.passed is False


def test_gt_l3_explicit_skip() -> None:
    result = run_gt_l3_check(skip_gt=True)
    assert result.status == "skipped"
    assert result.passed is True


def test_markdown_measured_gt_metrics() -> None:
    report = CutoverGateReport(
        parity_results=[],
        python_metrics=None,
        cpp_metrics=None,
        gt_result=GtL3Result(
            status="measured",
            passed=True,
            message="all ok",
            metrics=[
                GtL3MetricRow("timing_f1", 0.977, 0.952, True, True),
                GtL3MetricRow("cer_macro", 0.032, 0.066, False, True),
            ],
            elapsed_s=12.0,
        ),
        correctness_passed=True,
        runtime_passed=True,
        gt_passed=True,
        all_passed=True,
        runtime_skipped=True,
        gates_run=["correctness_parity", "gt_l3_measured"],
        gates_missing=["runtime_wall_cancel_restart_rss"],
    )
    md = generate_markdown_report(report)
    assert "timing_f1" in md
    assert "0.9770" in md
    assert "MEASURED" in md


def test_cli_cutover_gate_script_execution(tmp_path: Path) -> None:
    report_file = tmp_path / "cli_report.md"
    cmd = [
        sys.executable,
        "scripts/parity/check_cutover_gate.py",
        "--check",
        "--skip-runtime",
        "--skip-gt",
        "--report-out",
        str(report_file),
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr + proc.stdout
    assert "[PASS] Cutover 门禁验证全部通过！" in proc.stdout
    assert report_file.exists()


def test_cli_missing_worker_exits_nonzero_without_skip(tmp_path: Path) -> None:
    """CLI path: missing worker must exit 1 when not --skip-runtime."""
    report_file = tmp_path / "cli_fail.md"
    missing = tmp_path / "missing" / "sublift_worker"
    code = f"""
from pathlib import Path
import scripts.parity.check_cutover_gate as g

def _missing() -> Path:
    return Path({str(missing)!r})

g.worker_bin = _missing
g.resolve_worker_bin = lambda *a, **k: None
report = g.run_cutover_gate(
    check=True,
    skip_runtime=False,
    skip_gt=True,
    report_out=Path({str(report_file)!r}),
)
raise SystemExit(0 if report.all_passed else 1)
"""
    proc = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True)
    assert proc.returncode == 1, proc.stderr + proc.stdout
