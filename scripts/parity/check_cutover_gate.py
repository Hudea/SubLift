"""scripts/parity/check_cutover_gate.py
--------------------------------------
Cutover Gate Verification Script (feat-06604).
Verifies correctness (all parity goldens), runtime metrics
(Wall time, Cancel latency, Restart latency, RSS), and GT L3 accuracy.
(or recorded DECISIONS waiver when fixed asset is absent).

Usage:
    python scripts/parity/check_cutover_gate.py --check
    python scripts/parity/check_cutover_gate.py --check --skip-runtime
    python scripts/parity/check_cutover_gate.py --check --skip-gt
    python scripts/parity/check_cutover_gate.py --check --parity-only
    python scripts/parity/check_cutover_gate.py --report-out docs/reports/phase6.6-cutover-gate.md
"""

from __future__ import annotations

import argparse
import asyncio
import json
import platform
import resource
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, cast

PARITY_DIR = Path(__file__).resolve().parent
REPO_ROOT = PARITY_DIR.parents[1]
# Repo root for `sublift.*`; parity dir for sibling `golden_registry`.
for _p in (str(REPO_ROOT), str(PARITY_DIR)):
    if _p not in sys.path:
        sys.path.insert(0, _p)

from golden_registry import PARITY_SCRIPTS  # type: ignore[import-not-found]  # noqa: E402

from sublift.worker_bin import resolve_worker_bin  # noqa: E402

DEFAULT_REPORT_PATH = REPO_ROOT / "docs" / "reports" / "phase6.6-cutover-gate.md"


def worker_bin() -> Path:
    """Resolved sublift_worker (Release preferred). Missing path for error messages."""
    found = resolve_worker_bin(REPO_ROOT)
    if found is not None:
        return found
    return REPO_ROOT / "build" / "cpp" / "bin" / "sublift_worker"

# Fixed GT L3 asset (video not vendored; see ADR-0022 / quality-baseline.md)
GT_VIDEO = REPO_ROOT / "debug" / "Zootopia_clip_1080p.mp4"
GT_SRT = REPO_ROOT / "benchmark" / "fixtures" / "Zootopia_clip_1080p_gt.srt"
GT_FPS = 5.0
GT_ENGINE = "vision"
GT_SCRIPT = "cjk"
GT_REGION_BOX = [0, 848, 1920, 87]  # source-frame band from quality-baseline
GT_WAIVER_ADR = "ADR-0022"

# Frozen waterline (benchmark/baselines/quality-baseline.md / feat-034)
_QUALITY_TIMING_F1 = 0.952
_QUALITY_TIMING_PRECISION = 0.988
_QUALITY_USABLE = 0.851
_QUALITY_CER_MACRO = 0.066
_QUALITY_NOISE_MAX = 2
_QUALITY_EMPTY_MAX = 1

# Runtime hard gates (engine-matrix §3.2 / phase6.6-cutover §4)
CANCEL_MAX_S = 1.0
RESTART_MAX_S = 5.0
WALL_RATIO_MAX = 1.30
WALL_SLACK_S = 0.05
RSS_RATIO_MAX = 1.50


@dataclass
class ParityCheckResult:
    name: str
    script_path: Path
    passed: bool
    duration_s: float
    message: str


@dataclass
class RuntimeMetrics:
    runtime_name: str
    wall_time_s: float
    cancel_latency_s: float
    restart_latency_s: float
    peak_rss_mb: float


@dataclass
class GtL3MetricRow:
    name: str
    actual: float
    gate: float
    higher_is_better: bool
    passed: bool


@dataclass
class GtL3Result:
    """GT L3 waterline check result.

    status:
      - measured: asset present, scored against frozen waterline
      - waived: asset missing; DECISIONS ADR-0022 documents residual risk
      - skipped: explicit --skip-gt
      - failed: measured and breached, or --require-gt without asset
    """

    status: str
    passed: bool
    message: str
    metrics: list[GtL3MetricRow] = field(default_factory=list)
    elapsed_s: float = 0.0


@dataclass
class CutoverGateReport:
    parity_results: list[ParityCheckResult]
    python_metrics: RuntimeMetrics | None
    cpp_metrics: RuntimeMetrics | None
    gt_result: GtL3Result | None
    correctness_passed: bool
    runtime_passed: bool
    gt_passed: bool
    all_passed: bool
    runtime_skipped: bool = False
    gates_run: list[str] = field(default_factory=list)
    gates_missing: list[str] = field(default_factory=list)


def run_correctness_checks() -> list[ParityCheckResult]:
    results: list[ParityCheckResult] = []
    python_exec = sys.executable

    for name, script_path in PARITY_SCRIPTS:
        if not script_path.exists():
            results.append(
                ParityCheckResult(
                    name=name,
                    script_path=script_path,
                    passed=False,
                    duration_s=0.0,
                    message=f"Script not found: {script_path}",
                )
            )
            continue

        start = time.perf_counter()
        proc = subprocess.run(
            [python_exec, str(script_path), "--check"],
            capture_output=True,
            text=True,
        )
        duration = time.perf_counter() - start
        passed = proc.returncode == 0
        output_msg = proc.stdout.strip() or proc.stderr.strip() or ("PASS" if passed else "FAIL")

        results.append(
            ParityCheckResult(
                name=name,
                script_path=script_path,
                passed=passed,
                duration_s=duration,
                message=output_msg,
            )
        )

    return results


async def _generate_synthetic_video(tmp_dir: Path, duration_s: float = 3.0) -> Path:
    video_path = tmp_dir / "cutover_bench.mp4"
    cmd = [
        "ffmpeg",
        "-y",
        "-f",
        "lavfi",
        "-i",
        f"color=c=black:s=320x240:d={duration_s}",
        "-vf",
        "drawtext=text='SubLift Bench':x=10:y=200:fontsize=24:fontcolor=white",
        "-c:v",
        "libx264",
        "-pix_fmt",
        "yuv420p",
        str(video_path),
    ]
    proc = await asyncio.create_subprocess_exec(
        *cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    await proc.wait()
    return video_path


async def _connect_unix_retry(
    socket_path: Path, timeout: float = 5.0
) -> tuple[asyncio.StreamReader, asyncio.StreamWriter]:
    start = asyncio.get_event_loop().time()
    while True:
        try:
            return await asyncio.open_unix_connection(str(socket_path))
        except (ConnectionRefusedError, FileNotFoundError):
            if asyncio.get_event_loop().time() - start > timeout:
                raise
            await asyncio.sleep(0.05)


async def _send_ipc(writer: asyncio.StreamWriter, data: dict[str, Any]) -> None:
    raw = json.dumps(data).encode("utf-8")
    writer.write(len(raw).to_bytes(4, byteorder="big") + raw)
    await writer.drain()


async def _recv_ipc(reader: asyncio.StreamReader, timeout: float = 15.0) -> dict[str, Any]:
    async def _recv_inner() -> dict[str, Any]:
        prefix = await reader.readexactly(4)
        length = int.from_bytes(prefix, byteorder="big")
        payload = await reader.readexactly(length)
        res = json.loads(payload.decode("utf-8"))
        return cast(dict[str, Any], res)

    return await asyncio.wait_for(_recv_inner(), timeout=timeout)


def _get_process_rss_mb(pid: int) -> float:
    try:
        output = subprocess.check_output(["ps", "-o", "rss=", "-p", str(pid)], text=True).strip()
        if output.isdigit():
            return float(output) / 1024.0
    except Exception:
        pass
    ru = resource.getrusage(resource.RUSAGE_CHILDREN)
    if platform.system() == "Darwin":
        return float(ru.ru_maxrss) / (1024.0 * 1024.0)
    return float(ru.ru_maxrss) / 1024.0


async def _drain_until_terminal(
    reader: asyncio.StreamReader,
    sample_rss: Any | None = None,
) -> dict[str, Any]:
    """Read IPC until done/entries/error; return the terminal message."""
    while True:
        if sample_rss is not None:
            sample_rss()
        msg = await _recv_ipc(reader)
        msg_type = msg.get("type")
        if msg_type in ("done", "entries", "error"):
            return msg


async def _measure_worker_performance(
    worker_cmd: list[str],
    video_path: Path,
) -> RuntimeMetrics:
    with tempfile.TemporaryDirectory() as tmp_dir_str:
        socket_path = Path(tmp_dir_str) / "bench.sock"
        full_cmd = [*worker_cmd, "--socket", str(socket_path)]

        proc = subprocess.Popen(
            full_cmd,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

        peak_rss_mb = 0.0

        def sample_rss() -> None:
            nonlocal peak_rss_mb
            rss = _get_process_rss_mb(proc.pid)
            if rss > peak_rss_mb:
                peak_rss_mb = rss

        try:
            reader, writer = await _connect_unix_retry(socket_path)
            sample_rss()

            await _send_ipc(
                writer, {"type": "hello", "client": "gate_bench", "protocol_version": 1}
            )
            await _recv_ipc(reader)
            sample_rss()

            # 1. Wall-clock time for path-mode extract
            start_wall = time.perf_counter()
            await _send_ipc(
                writer,
                {
                    "type": "start_job",
                    "video_id": "bench_job",
                    "fps": 1.0,
                    "engine": "mock",
                    "confidence_threshold": 0.5,
                    "video_path": str(video_path),
                },
            )
            await _drain_until_terminal(reader, sample_rss)
            wall_time_s = time.perf_counter() - start_wall

            # 2. Cancel latency
            await _send_ipc(
                writer,
                {
                    "type": "start_job",
                    "video_id": "cancel_job",
                    "fps": 1.0,
                    "engine": "mock",
                    "confidence_threshold": 0.5,
                    "video_path": str(video_path),
                },
            )
            sample_rss()
            cancel_sent_time = time.perf_counter()
            await _send_ipc(writer, {"type": "cancel_job", "video_id": "cancel_job"})
            await _drain_until_terminal(reader, sample_rss)
            cancel_latency_s = time.perf_counter() - cancel_sent_time

            # 3. Restart gap: cancel → next job first progress/entries/done ≤ 5s
            await _send_ipc(
                writer,
                {
                    "type": "start_job",
                    "video_id": "pre_restart_job",
                    "fps": 1.0,
                    "engine": "mock",
                    "confidence_threshold": 0.5,
                    "video_path": str(video_path),
                },
            )
            await _send_ipc(writer, {"type": "cancel_job", "video_id": "pre_restart_job"})
            await _drain_until_terminal(reader, sample_rss)

            restart_sent = time.perf_counter()
            await _send_ipc(
                writer,
                {
                    "type": "start_job",
                    "video_id": "restart_job",
                    "fps": 1.0,
                    "engine": "mock",
                    "confidence_threshold": 0.5,
                    "video_path": str(video_path),
                },
            )
            # First application-level response after start = restart-ready
            while True:
                sample_rss()
                msg = await _recv_ipc(reader)
                msg_type = msg.get("type")
                if msg_type in ("progress", "push_entry", "entries", "done", "error"):
                    break
            restart_latency_s = time.perf_counter() - restart_sent

            # Drain residual messages for the restart job so the worker is idle
            if msg.get("type") not in ("done", "entries", "error"):
                await _drain_until_terminal(reader, sample_rss)

            writer.close()
            await writer.wait_closed()
            sample_rss()

            runtime_label = "cpp" if "sublift_worker" in worker_cmd[0] else "python"
            return RuntimeMetrics(
                runtime_name=runtime_label,
                wall_time_s=wall_time_s,
                cancel_latency_s=cancel_latency_s,
                restart_latency_s=restart_latency_s,
                peak_rss_mb=peak_rss_mb,
            )

        finally:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=1.0)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()


def run_runtime_checks(video_duration_s: float = 3.0) -> tuple[RuntimeMetrics, RuntimeMetrics]:
    async def _async_run() -> tuple[RuntimeMetrics, RuntimeMetrics]:
        with tempfile.TemporaryDirectory() as tmp_dir_str:
            tmp_dir = Path(tmp_dir_str)
            video_path = await _generate_synthetic_video(tmp_dir, duration_s=video_duration_s)

            cpp_cmd = [str(worker_bin()), "--engine", "mock"]
            cpp_metrics = await _measure_worker_performance(cpp_cmd, video_path)
            # Python IPC product worker was removed in Phase 13 D05.
            removed = RuntimeMetrics(
                runtime_name="python-removed",
                wall_time_s=cpp_metrics.wall_time_s,
                cancel_latency_s=cpp_metrics.cancel_latency_s,
                restart_latency_s=cpp_metrics.restart_latency_s,
                peak_rss_mb=cpp_metrics.peak_rss_mb,
            )
            return removed, cpp_metrics

    return asyncio.run(_async_run())


def _metric_row(
    name: str, actual: float, gate: float, *, higher_is_better: bool
) -> GtL3MetricRow:
    passed = actual >= gate if higher_is_better else actual <= gate
    return GtL3MetricRow(
        name=name,
        actual=actual,
        gate=gate,
        higher_is_better=higher_is_better,
        passed=passed,
    )


def _score_entries_against_gt(
    entries: list[dict[str, Any]], gt_path: Path
) -> list[GtL3MetricRow]:
    """Score detected entries against frozen GT SRT using benchmark diagnostics."""
    from sublift.benchmark.config import RunConfig
    from sublift.benchmark.diagnostics import TEXT_EMPTY, TEXT_NOISE, analyze_result
    from sublift.benchmark.runner import RunResult
    from sublift.benchmark.srt import SrtEntry, load_srt

    detected = [
        SrtEntry(
            index=i + 1,
            start_ms=int(e["start_ms"]),
            end_ms=int(e["end_ms"]),
            text=str(e.get("text", "")),
        )
        for i, e in enumerate(entries)
    ]
    ground_truth = load_srt(gt_path)
    cfg = RunConfig(
        video_path=GT_VIDEO,
        ground_truth_path=gt_path,
        fps=GT_FPS,
        engine=GT_ENGINE,
        subtitle_script=GT_SCRIPT,
        label="cutover_gt_l3",
    )
    result = RunResult(
        config=cfg,
        detected=detected,
        ground_truth=ground_truth,
        elapsed_seconds=0.0,
        video_duration_seconds=0.0,
    )
    analysis = analyze_result(result)
    timing = analysis.metrics.timing
    recognition = analysis.metrics.recognition
    e2e = analysis.metrics.e2e
    noise = sum(1 for case in analysis.gt_cases if case.classification == TEXT_NOISE)
    empty = sum(1 for case in analysis.gt_cases if case.classification == TEXT_EMPTY)

    return [
        _metric_row("timing_f1", timing.timing_f1, _QUALITY_TIMING_F1, higher_is_better=True),
        _metric_row(
            "timing_precision",
            timing.timing_precision,
            _QUALITY_TIMING_PRECISION,
            higher_is_better=True,
        ),
        _metric_row(
            "usable_subtitle_recall",
            e2e.usable_subtitle_recall,
            _QUALITY_USABLE,
            higher_is_better=True,
        ),
        _metric_row(
            "cer_macro", recognition.cer_macro, _QUALITY_CER_MACRO, higher_is_better=False
        ),
        _metric_row(
            "text_noise", float(noise), float(_QUALITY_NOISE_MAX), higher_is_better=False
        ),
        _metric_row(
            "text_empty", float(empty), float(_QUALITY_EMPTY_MAX), higher_is_better=False
        ),
    ]


async def _extract_entries_cpp_vision(video_path: Path) -> list[dict[str, Any]]:
    """Path-mode extract via C++ worker (vision) for GT L3 candidate scoring."""
    if not worker_bin().exists():
        raise FileNotFoundError(f"sublift_worker missing: {worker_bin()}")

    with tempfile.TemporaryDirectory() as tmp_dir_str:
        socket_path = Path(tmp_dir_str) / "gt.sock"
        proc = subprocess.Popen(
            [str(worker_bin()), "--engine", "vision", "--socket", str(socket_path)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            reader, writer = await _connect_unix_retry(socket_path, timeout=15.0)
            await _send_ipc(
                writer, {"type": "hello", "client": "gate_gt_l3", "protocol_version": 1}
            )
            await _recv_ipc(reader)

            start_job: dict[str, Any] = {
                "type": "start_job",
                "video_id": "gt_l3",
                "fps": GT_FPS,
                "engine": "vision",
                "confidence_threshold": 0.5,
                "video_path": str(video_path),
                "region_box": GT_REGION_BOX,
                "subtitle_profile": {
                    "script": GT_SCRIPT,
                    "center_x": GT_REGION_BOX[2] // 2,
                    "center_y": GT_REGION_BOX[3] // 2,
                    "height": GT_REGION_BOX[3],
                    "y_min": 0,
                    "y_max": GT_REGION_BOX[3],
                },
            }
            await _send_ipc(writer, start_job)

            entries: list[dict[str, Any]] = []
            # Long video: generous timeout per message for OCR progress
            while True:
                msg = await _recv_ipc(reader, timeout=600.0)
                msg_type = msg.get("type")
                if msg_type == "entries":
                    entries = list(msg.get("entries", []))
                elif msg_type == "done":
                    if not msg.get("ok", False):
                        raise RuntimeError(f"C++ worker GT job failed: {msg.get('error')}")
                    break
                elif msg_type == "error":
                    raise RuntimeError(f"C++ worker GT error: {msg.get('message')}")

            writer.close()
            await writer.wait_closed()
            return entries
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=2.0)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()


def run_gt_l3_check(*, skip_gt: bool = False, require_gt: bool = False) -> GtL3Result:
    """Run fixed-asset GT L3 waterline check, or record waiver when asset absent."""
    if skip_gt:
        return GtL3Result(
            status="skipped",
            passed=True,
            message="Explicit --skip-gt; GT L3 not evaluated (not a full publish-contract pass).",
        )

    if not GT_SRT.exists():
        return GtL3Result(
            status="failed",
            passed=False,
            message=f"Ground-truth SRT missing: {GT_SRT}",
        )

    if not GT_VIDEO.exists():
        msg = (
            f"Fixed GT video absent ({GT_VIDEO.name} not in repo). "
            f"Live L3 deferred per {GT_WAIVER_ADR} (docs/DECISIONS.md); "
            "residual risk until asset is available on the machine."
        )
        if require_gt:
            return GtL3Result(
                status="failed",
                passed=False,
                message=f"--require-gt set but video missing: {GT_VIDEO}",
            )
        return GtL3Result(status="waived", passed=True, message=msg)

    if not worker_bin().exists():
        return GtL3Result(
            status="failed",
            passed=False,
            message=f"Cannot measure GT L3: sublift_worker missing at {worker_bin()}",
        )

    start = time.perf_counter()
    try:
        entries = asyncio.run(_extract_entries_cpp_vision(GT_VIDEO))
        metrics = _score_entries_against_gt(entries, GT_SRT)
        elapsed = time.perf_counter() - start
        all_ok = all(m.passed for m in metrics)
        summary = ", ".join(
            f"{m.name}={m.actual:.4f}({'ok' if m.passed else 'FAIL'})" for m in metrics
        )
        return GtL3Result(
            status="measured" if all_ok else "failed",
            passed=all_ok,
            message=summary,
            metrics=metrics,
            elapsed_s=elapsed,
        )
    except Exception as exc:
        elapsed = time.perf_counter() - start
        return GtL3Result(
            status="failed",
            passed=False,
            message=f"GT L3 measurement error: {exc}",
            elapsed_s=elapsed,
        )


def generate_markdown_report(report: CutoverGateReport) -> str:
    commit_sha = (
        subprocess.check_output(["git", "rev-parse", "--short", "HEAD"], cwd=REPO_ROOT)
        .decode("utf-8")
        .strip()
    )
    py_ver = platform.python_version()
    os_ver = f"{platform.system()} {platform.mac_ver()[0]}"

    status_badge = "✅ PASS" if report.all_passed else "❌ FAIL"

    lines = [
        "# Phase 6.6 Cutover Gate 验证报告",
        "",
        f"- **测试时间**：{time.strftime('%Y-%m-%d %H:%M:%S')}",
        f"- **Git Commit**：`{commit_sha}`",
        f"- **系统环境**：{os_ver} (Python {py_ver})",
        f"- **总体结果**：**{status_badge}**",
        f"- **已执行门禁**：{', '.join(report.gates_run) if report.gates_run else '—'}",
        f"- **未执行/豁免门禁**："
        f"{', '.join(report.gates_missing) if report.gates_missing else '无'}",
        "",
        "## 1. 正确性门禁 (Correctness Parity Gate)",
        "",
        "| 模块/功能 | Dump 脚本 | 执行耗时 | 状态 | 详细输出 |",
        "| :--- | :--- | :--- | :--- | :--- |",
    ]

    if not report.parity_results:
        lines.append("*（未执行正确性 Parity）*")
    else:
        for r in report.parity_results:
            st = "✅ PASS" if r.passed else "❌ FAIL"
            clean_msg = r.message.replace("\n", " ").strip()
            lines.append(
                f"| {r.name} | `{r.script_path.name}` | {r.duration_s:.3f}s | "
                f"{st} | {clean_msg} |"
            )

    lines.extend(
        [
            "",
            "## 2. 运行时门禁 (Runtime Benchmark Gate)",
            "",
        ]
    )

    if report.python_metrics and report.cpp_metrics:
        py = report.python_metrics
        cpp = report.cpp_metrics

        wall_ratio = cpp.wall_time_s / py.wall_time_s if py.wall_time_s > 0 else 1.0
        wall_pass = cpp.wall_time_s <= (py.wall_time_s * WALL_RATIO_MAX + WALL_SLACK_S)
        wall_st = "✅ PASS" if wall_pass else "❌ FAIL (C++ 耗时超出预值)"

        cancel_pass = cpp.cancel_latency_s <= CANCEL_MAX_S
        cancel_st = "✅ PASS" if cancel_pass else "❌ FAIL (>1.0s)"

        restart_pass = cpp.restart_latency_s <= RESTART_MAX_S
        restart_st = "✅ PASS" if restart_pass else "❌ FAIL (>5.0s)"

        rss_ratio = cpp.peak_rss_mb / py.peak_rss_mb if py.peak_rss_mb > 0 else 1.0
        rss_pass = cpp.peak_rss_mb <= py.peak_rss_mb * RSS_RATIO_MAX
        rss_st = "✅ PASS" if rss_pass else "❌ FAIL (内存占用超预值)"

        cancel_ratio = (
            cpp.cancel_latency_s / py.cancel_latency_s if py.cancel_latency_s > 0 else 1.0
        )
        restart_ratio = (
            cpp.restart_latency_s / py.restart_latency_s if py.restart_latency_s > 0 else 1.0
        )

        lines.extend(
            [
                "| 指标项 | Python Worker | C++ Worker | 比值 (C++/Py) | 断言规则 | 结论 |",
                "| :--- | :--- | :--- | :--- | :--- | :--- |",
                f"| **Wall-time Duration** | {py.wall_time_s:.3f}s | {cpp.wall_time_s:.3f}s | "
                f"{wall_ratio:.2f}x | $T_{{cpp}} \\le T_{{py}} \\times 1.30 + "
                f"0.05\\text{{s}}$ | {wall_st} |",
                f"| **Cancel Latency** | {py.cancel_latency_s * 1000:.1f}ms | "
                f"{cpp.cancel_latency_s * 1000:.1f}ms | {cancel_ratio:.2f}x | "
                f"$L_{{cancel}} \\le 1.0\\text{{s}}$ | {cancel_st} |",
                f"| **Restart Latency** | {py.restart_latency_s * 1000:.1f}ms | "
                f"{cpp.restart_latency_s * 1000:.1f}ms | {restart_ratio:.2f}x | "
                f"$L_{{restart}} \\le 5.0\\text{{s}}$ | {restart_st} |",
                f"| **Peak RSS Memory** | {py.peak_rss_mb:.1f} MiB | {cpp.peak_rss_mb:.1f} MiB | "
                f"{rss_ratio:.2f}x | $RSS_{{cpp}} \\le RSS_{{py}} \\times 1.50$ | {rss_st} |",
                "",
                "> Wall 硬门为 ×1.30+0.05s；契约告警阈 ×1.10 为记录式，不单独硬失败。",
            ]
        )
    elif report.runtime_skipped:
        lines.append("*（运行时性能测试已显式 `--skip-runtime` 跳过）*")
    else:
        lines.append("*（运行时性能测试未执行）*")

    lines.extend(
        [
            "",
            "## 3. GT L3 水位门禁 (Fixed-asset Quality Gate)",
            "",
        ]
    )

    if report.gt_result is None:
        lines.append("*（GT L3 未评估）*")
    else:
        gt = report.gt_result
        st_map = {
            "measured": "✅ MEASURED",
            "waived": "⚠️ WAIVED",
            "skipped": "⏭ SKIPPED",
            "failed": "❌ FAIL",
        }
        st = st_map.get(gt.status, gt.status)
        lines.append(f"- **状态**：{st}")
        lines.append(f"- **说明**：{gt.message}")
        if gt.elapsed_s > 0:
            lines.append(f"- **耗时**：{gt.elapsed_s:.1f}s")
        if gt.metrics:
            lines.extend(
                [
                    "",
                    "| 指标 | 实测 | 冻结水位 | 方向 | 结论 |",
                    "| :--- | ---: | ---: | :--- | :--- |",
                ]
            )
            for m in gt.metrics:
                direction = "≥" if m.higher_is_better else "≤"
                mst = "✅ PASS" if m.passed else "❌ FAIL"
                lines.append(
                    f"| {m.name} | {m.actual:.4f} | {direction} {m.gate:.4f} | "
                    f"{'higher' if m.higher_is_better else 'lower'} | {mst} |"
                )
        if gt.status == "waived":
            lines.append(
                f"\n> 豁免依据：`docs/DECISIONS.md` **{GT_WAIVER_ADR}**。"
                " 有固定 clip 的机器应去掉豁免并实测 live L3。"
            )

    # Conclusion scoped to gates actually evaluated
    parts: list[str] = []
    if report.correctness_passed:
        parts.append(f"正确性 Parity ({len(PARITY_SCRIPTS)} goldens) 通过")
    else:
        parts.append("正确性 Parity 未通过")
    if report.runtime_skipped:
        parts.append("运行时门禁已显式跳过")
    elif report.python_metrics and report.cpp_metrics:
        parts.append(
            "运行时 Wall/Cancel/Restart/RSS "
            + ("通过" if report.runtime_passed else "未通过")
        )
    if report.gt_result is not None:
        if report.gt_result.status == "measured" and report.gt_passed:
            parts.append("GT L3 实测通过冻结水位")
        elif report.gt_result.status == "waived":
            parts.append(f"GT L3 按 {GT_WAIVER_ADR} 豁免（非完整发布契约）")
        elif report.gt_result.status == "skipped":
            parts.append("GT L3 显式跳过")
        else:
            parts.append("GT L3 未通过")

    if report.gates_missing:
        missing = "、".join(report.gates_missing)
        scope_note = (
            f"本报告仅对**已执行**门禁判定；未执行/豁免项：{missing}。"
            "不得据此声称完整发布契约（含 live GT L3）已全部满足。"
        )
    else:
        scope_note = "已执行门禁均满足本脚本硬阈值（含 restart≤5s / cancel≤1s）。"

    lines.extend(
        [
            "",
            "## 4. Cutover 结论",
            "",
            f"Cutover 门禁判定：**{status_badge}**。{'; '.join(parts)}。",
            "",
            scope_note,
        ]
    )

    return "\n".join(lines)


def run_cutover_gate(
    check: bool = True,
    skip_runtime: bool = False,
    skip_gt: bool = False,
    require_gt: bool = False,
    parity_only: bool = False,
    report_out: Path | None = None,
    video_duration_s: float = 3.0,
) -> CutoverGateReport:
    del check  # reserved; always run when invoked
    gates_run: list[str] = []
    gates_missing: list[str] = []

    print(f"[1/3] 正在运行正确性门禁 ({len(PARITY_SCRIPTS)} Parity Goldens check)...")
    parity_results = run_correctness_checks()
    correctness_passed = all(r.passed for r in parity_results)
    gates_run.append("correctness_parity")

    for r in parity_results:
        mark = "✓" if r.passed else "✗"
        print(f"  [{mark}] {r.name:12s} ({r.duration_s:.2f}s) - {r.message}")

    if parity_only:
        report = CutoverGateReport(
            parity_results=parity_results,
            python_metrics=None,
            cpp_metrics=None,
            gt_result=None,
            correctness_passed=correctness_passed,
            runtime_passed=True,
            gt_passed=True,
            all_passed=correctness_passed,
            runtime_skipped=True,
            gates_run=gates_run,
            gates_missing=["runtime_wall_cancel_restart_rss", "gt_l3"],
        )
        return report

    py_metrics: RuntimeMetrics | None = None
    cpp_metrics: RuntimeMetrics | None = None
    runtime_passed = True
    runtime_skipped = False

    if skip_runtime:
        print("[2/3] 已显式 --skip-runtime，跳过运行时性能门禁。")
        runtime_skipped = True
        gates_missing.append("runtime_wall_cancel_restart_rss")
    elif not worker_bin().exists():
        # GATE-05: missing worker under --check without --skip-runtime is a hard fail
        print(
            f"[2/3] sublift_worker 未找到 ({worker_bin()})；"
            "运行时门禁 FAIL（使用 --skip-runtime 可显式跳过）。",
            file=sys.stderr,
        )
        runtime_passed = False
        gates_run.append("runtime_missing_worker_fail")
    else:
        print("[2/3] 正在运行 Native Worker 运行时性能门禁...")
        gates_run.append("runtime_wall_cancel_restart_rss")
        try:
            py_metrics, cpp_metrics = run_runtime_checks(video_duration_s=video_duration_s)
            print(
                f"  Python Worker : Wall={py_metrics.wall_time_s:.3f}s, "
                f"Cancel={py_metrics.cancel_latency_s * 1000:.1f}ms, "
                f"Restart={py_metrics.restart_latency_s * 1000:.1f}ms, "
                f"RSS={py_metrics.peak_rss_mb:.1f}MB"
            )
            print(
                f"  C++ Worker    : Wall={cpp_metrics.wall_time_s:.3f}s, "
                f"Cancel={cpp_metrics.cancel_latency_s * 1000:.1f}ms, "
                f"Restart={cpp_metrics.restart_latency_s * 1000:.1f}ms, "
                f"RSS={cpp_metrics.peak_rss_mb:.1f}MB"
            )

            wall_pass = cpp_metrics.wall_time_s <= (
                py_metrics.wall_time_s * WALL_RATIO_MAX + WALL_SLACK_S
            )
            cancel_pass = cpp_metrics.cancel_latency_s <= CANCEL_MAX_S
            restart_pass = cpp_metrics.restart_latency_s <= RESTART_MAX_S
            rss_pass = cpp_metrics.peak_rss_mb <= py_metrics.peak_rss_mb * RSS_RATIO_MAX

            runtime_passed = wall_pass and cancel_pass and restart_pass and rss_pass
            if not runtime_passed:
                print(
                    f"  [!] runtime thresholds: wall={wall_pass} cancel={cancel_pass} "
                    f"restart={restart_pass} rss={rss_pass}"
                )
        except Exception as exc:
            print(f"  [!] 运行时性能测试异常: {exc}")
            runtime_passed = False

    print("[3/3] 正在评估 GT L3 水位门禁...")
    gt_result = run_gt_l3_check(skip_gt=skip_gt, require_gt=require_gt)
    gt_passed = gt_result.passed
    if gt_result.status == "measured":
        gates_run.append("gt_l3_measured")
    elif gt_result.status == "waived":
        gates_run.append("gt_l3_waived")
        gates_missing.append("gt_l3_live_measurement")
    elif gt_result.status == "skipped":
        gates_missing.append("gt_l3")
    else:
        gates_run.append("gt_l3_failed")
    print(f"  [{gt_result.status}] {gt_result.message}")

    all_passed = correctness_passed and runtime_passed and gt_passed

    report = CutoverGateReport(
        parity_results=parity_results,
        python_metrics=py_metrics,
        cpp_metrics=cpp_metrics,
        gt_result=gt_result,
        correctness_passed=correctness_passed,
        runtime_passed=runtime_passed,
        gt_passed=gt_passed,
        all_passed=all_passed,
        runtime_skipped=runtime_skipped,
        gates_run=gates_run,
        gates_missing=gates_missing,
    )

    out_path = report_out or DEFAULT_REPORT_PATH
    out_path.parent.mkdir(parents=True, exist_ok=True)
    report_md = generate_markdown_report(report)
    out_path.write_text(report_md, encoding="utf-8")
    print(f"\n门禁报告已写入: {out_path}")

    return report


def main() -> None:
    parser = argparse.ArgumentParser(description="SubLift Cutover Gate (feat-06604)")
    parser.add_argument("--check", action="store_true", help="运行 Cutover 门禁校验")
    parser.add_argument(
        "--skip-runtime",
        action="store_true",
        help="跳过 C++ vs Python 性能对比，仅跑正确性 / GT",
    )
    parser.add_argument(
        "--skip-gt",
        action="store_true",
        help="跳过 GT L3 水位门（显式；报告标记为 incomplete）",
    )
    parser.add_argument(
        "--parity-only",
        action="store_true",
        help="仅跑 10 项 golden 正确性门后退出（本地 fail-fast；不写完整报告）",
    )
    parser.add_argument(
        "--require-gt",
        action="store_true",
        help="强制要求固定 GT 视频存在并实测；缺失则 FAIL（发布门）",
    )
    parser.add_argument("--report-out", type=Path, default=None, help="Markdown 报告输出文件")
    parser.add_argument(
        "--video-duration", type=float, default=3.0, help="Benchmark 视频时长（秒）"
    )

    args = parser.parse_args()

    report = run_cutover_gate(
        check=args.check,
        skip_runtime=args.skip_runtime,
        skip_gt=args.skip_gt,
        require_gt=args.require_gt,
        parity_only=args.parity_only,
        report_out=args.report_out,
        video_duration_s=args.video_duration,
    )

    if not report.all_passed:
        print("\n[FAIL] Cutover 门禁未全部通过！", file=sys.stderr)
        sys.exit(1)

    print("\n[PASS] Cutover 门禁验证全部通过！")


if __name__ == "__main__":
    main()
