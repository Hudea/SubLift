"""Real Paddle Python/C++ product performance gate for Phase 6.8.

The gate runs ``sublift extract`` for both runtimes on one hash-frozen
two-minute source.  It performs warm-up first, alternates measured runtime
order, and uses the median of at least three runs.  Wall time, user/sys CPU
time, process-tree RSS, output hash, worker hash and the exact ONNX Runtime
binary are recorded.  Missing assets or runtime dependencies fail closed.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import signal
import statistics
import subprocess
import sys
import tempfile
import threading
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any, cast

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))
DEFAULT_MANIFEST = (
    REPO_ROOT / "benchmark/datasets/paddle_performance/manifest.v1.json"
)


@dataclass
class PaddlePerfMetrics:
    wall_time_sec: float = 0.0
    peak_rss_mb: float = 0.0
    user_time_sec: float = 0.0
    system_time_sec: float = 0.0
    wall_min_sec: float = 0.0
    wall_max_sec: float = 0.0
    peak_rss_max_mb: float = 0.0
    output_sha256: str = ""
    output_entries: int = 0
    measured_run_count: int = 0
    ocr_call_count: int = 0
    det_box_count: int | None = None
    cls_batch_count: int | None = None
    rec_batch_count: int | None = None


@dataclass
class PerfGateResult:
    passed: bool
    name: str
    oracle_val: float
    candidate_val: float
    ratio: float
    limit: float
    message: str


@dataclass(frozen=True)
class TimedProductRun:
    runtime: str
    phase: str
    iteration: int
    wall_time_sec: float
    user_time_sec: float
    system_time_sec: float
    peak_tree_rss_mb: float
    output_sha256: str
    output_entries: int
    ocr_calls: int
    recognize_calls: int | None
    det_boxes: int | None
    cls_batches: int | None
    rec_batches: int | None


@dataclass
class PaddlePerfReport:
    overall_passed: bool
    metrics_oracle: PaddlePerfMetrics
    metrics_candidate: PaddlePerfMetrics
    gate_results: list[PerfGateResult] = field(default_factory=list)
    runs: list[TimedProductRun] = field(default_factory=list)
    environment: dict[str, Any] = field(default_factory=dict)
    failure: str | None = None


def _ratio(candidate: float, oracle: float) -> float:
    return candidate / oracle if oracle > 0.0 else float("inf")


def evaluate_paddle_perf_hard_gates(
    oracle: PaddlePerfMetrics,
    candidate: PaddlePerfMetrics,
    quality_passed: bool = True,
    *,
    product_wall_limit: float = 1.20,
    accepted_wall_limit: float = 1.00,
    rss_limit: float = 1.25,
) -> PaddlePerfReport:
    """Evaluate both the product floor and the user's stricter acceptance."""
    gates: list[PerfGateResult] = []

    wall_ratio = _ratio(candidate.wall_time_sec, oracle.wall_time_sec)
    for name, limit in (
        ("product_wall_time_ratio", product_wall_limit),
        ("accepted_faster_than_python", accepted_wall_limit),
    ):
        passed = wall_ratio <= limit
        gates.append(
            PerfGateResult(
                passed=passed,
                name=name,
                oracle_val=oracle.wall_time_sec,
                candidate_val=candidate.wall_time_sec,
                ratio=wall_ratio,
                limit=limit,
                message=(
                    "PASS"
                    if passed
                    else f"FAIL (wall ratio {wall_ratio:.4f}x > {limit:.4f}x)"
                ),
            )
        )

    rss_ratio = _ratio(candidate.peak_rss_mb, oracle.peak_rss_mb)
    rss_passed = rss_ratio <= rss_limit
    gates.append(
        PerfGateResult(
            passed=rss_passed,
            name="peak_process_tree_rss_ratio",
            oracle_val=oracle.peak_rss_mb,
            candidate_val=candidate.peak_rss_mb,
            ratio=rss_ratio,
            limit=rss_limit,
            message=(
                "PASS"
                if rss_passed
                else f"FAIL (RSS ratio {rss_ratio:.4f}x > {rss_limit:.4f}x)"
            ),
        )
    )

    quality_message = (
        "PASS"
        if quality_passed
        else "FAIL (measured output hash differs from the frozen quality output)"
    )
    gates.append(
        PerfGateResult(
            passed=quality_passed,
            name="quality_output_sha256_exact",
            oracle_val=1.0,
            candidate_val=1.0 if quality_passed else 0.0,
            ratio=1.0 if quality_passed else 0.0,
            limit=1.0,
            message=quality_message,
        )
    )

    return PaddlePerfReport(
        overall_passed=all(gate.passed for gate in gates),
        metrics_oracle=oracle,
        metrics_candidate=candidate,
        gate_results=gates,
    )


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return f"sha256:{digest.hexdigest()}"


def _load_manifest(path: Path) -> dict[str, Any]:
    if not path.is_file():
        raise RuntimeError(f"Paddle performance manifest is missing: {path}")
    payload = json.loads(path.read_text(encoding="utf-8"))
    if payload.get("schema_version") != 1:
        raise RuntimeError("unsupported Paddle performance manifest schema")
    source = payload.get("source")
    if not isinstance(source, dict):
        raise RuntimeError("Paddle performance manifest source is missing")
    video = REPO_ROOT / str(source["video"])
    if not video.is_file():
        raise RuntimeError(f"canonical Paddle performance video is missing: {video}")
    actual_video_sha = _sha256(video)
    if actual_video_sha != source["video_sha256"]:
        raise RuntimeError(
            "canonical Paddle performance video hash mismatch: "
            f"{actual_video_sha} != {source['video_sha256']}"
        )
    return cast(dict[str, Any], payload)


def _python_ort_library() -> Path:
    import onnxruntime as ort  # type: ignore[import-untyped]

    capi_dir = Path(ort.__file__).resolve().parent / "capi"
    candidates = sorted(capi_dir.glob("libonnxruntime*.dylib"))
    if not candidates:
        candidates = sorted(capi_dir.glob("libonnxruntime.so*"))
    if not candidates:
        raise RuntimeError("Python ONNX Runtime shared library could not be located")
    versioned = [path for path in candidates if path.is_file() and not path.is_symlink()]
    return max(versioned or candidates, key=lambda path: len(path.name))


def _resolve_worker(explicit: Path | None) -> Path:
    candidates: list[Path] = []
    if explicit is not None:
        candidates.append(explicit)
    env_path = os.environ.get("SUBLIFT_WORKER_PATH")
    if env_path:
        candidates.append(Path(env_path))
    candidates.extend(
        [
            REPO_ROOT / "build/cpp-rel/bin/sublift_worker",
            REPO_ROOT / "build/cpp/bin/sublift_worker",
        ]
    )
    for candidate in candidates:
        resolved = candidate.expanduser().resolve()
        if resolved.is_file() and os.access(resolved, os.X_OK):
            return resolved
    raise RuntimeError("Release C++ Paddle worker is missing or not executable")


def _resolve_candidate_ort(
    worker: Path,
    explicit: Path | None,
) -> Path:
    if explicit is not None:
        # Preserve a versioned/ABI-name symlink: @rpath may request
        # libonnxruntime.1.dylib while the physical wheel file is .1.28.0.
        candidate = explicit.expanduser().absolute()
        if not candidate.is_file():
            raise RuntimeError(f"candidate ONNX Runtime library is missing: {candidate}")
        return candidate

    if platform.system() == "Darwin":
        linked = subprocess.run(
            ["otool", "-L", str(worker)],
            capture_output=True,
            text=True,
            check=True,
        ).stdout
        for line in linked.splitlines()[1:]:
            dependency = line.strip().split(" (", 1)[0]
            if "libonnxruntime" in dependency and dependency.startswith("/"):
                candidate = Path(dependency).resolve()
                if candidate.is_file():
                    return candidate
    raise RuntimeError(
        "candidate ONNX Runtime cannot be resolved; pass --candidate-ort-library"
    )


def _process_tree_rss_mb(root_pid: int) -> float:
    completed = subprocess.run(
        ["ps", "-axo", "pid=,ppid=,rss="],
        capture_output=True,
        text=True,
        check=False,
    )
    if completed.returncode != 0:
        return 0.0
    rows: dict[int, tuple[int, int]] = {}
    for line in completed.stdout.splitlines():
        fields = line.split()
        if len(fields) != 3 or not all(item.isdigit() for item in fields):
            continue
        pid, ppid, rss_kib = (int(item) for item in fields)
        rows[pid] = (ppid, rss_kib)

    descendants = {root_pid}
    changed = True
    while changed:
        changed = False
        for pid, (ppid, _) in rows.items():
            if ppid in descendants and pid not in descendants:
                descendants.add(pid)
                changed = True
    return sum(rows.get(pid, (0, 0))[1] for pid in descendants) / 1024.0


_DARWIN_USER_RE = re.compile(r"(?m)^user\s+([0-9]+(?:\.[0-9]+)?)\s*$")
_DARWIN_SYS_RE = re.compile(r"(?m)^sys\s+([0-9]+(?:\.[0-9]+)?)\s*$")
_LINUX_USER_RE = re.compile(
    r"(?m)^\s*User time \(seconds\):\s*([0-9]+(?:\.[0-9]+)?)\s*$"
)
_LINUX_SYS_RE = re.compile(
    r"(?m)^\s*System time \(seconds\):\s*([0-9]+(?:\.[0-9]+)?)\s*$"
)
_PERF_STATS_RE = re.compile(r"(?m)^SUBLIFT_PADDLE_PERF_STATS\s+(.+)$")


def _parse_cpu_times(stderr: str) -> tuple[float, float]:
    if platform.system() == "Darwin":
        user_match = _DARWIN_USER_RE.search(stderr)
        sys_match = _DARWIN_SYS_RE.search(stderr)
    else:
        user_match = _LINUX_USER_RE.search(stderr)
        sys_match = _LINUX_SYS_RE.search(stderr)
    if user_match is None or sys_match is None:
        raise RuntimeError("/usr/bin/time user/sys metrics are missing")
    return float(user_match.group(1)), float(sys_match.group(1))


def _parse_runtime_stats(
    stderr: str,
    *,
    runtime: str,
) -> dict[str, int | None]:
    matches = _PERF_STATS_RE.findall(stderr)
    if not matches:
        raise RuntimeError(f"Paddle {runtime} OCR call diagnostics are missing")
    parsed_matches: list[dict[str, int]] = []
    for match in matches:
        parsed: dict[str, int] = {}
        for token in match.split():
            key, separator, value = token.partition("=")
            if separator and value.isdigit():
                parsed[key] = int(value)
        parsed_matches.append(parsed)
    values = parsed_matches[-1]
    if "ocr_calls" not in values:
        raise RuntimeError(f"Paddle {runtime} ocr_calls diagnostic is missing")
    if runtime == "cpp":
        required = {"recognize_calls", "det_boxes", "cls_batches", "rec_batches"}
        for parsed in reversed(parsed_matches):
            if required.issubset(parsed):
                values = parsed
                break
        missing = required.difference(values)
        if missing:
            raise RuntimeError(
                "Paddle C++ runtime diagnostics are incomplete: "
                f"{sorted(missing)}; markers={matches!r}"
            )
        if values["ocr_calls"] != values["recognize_calls"]:
            raise RuntimeError(
                "Paddle C++ pipeline/engine OCR call counts disagree"
            )
    return {
        "ocr_calls": values["ocr_calls"],
        "recognize_calls": values.get("recognize_calls"),
        "det_boxes": values.get("det_boxes"),
        "cls_batches": values.get("cls_batches"),
        "rec_batches": values.get("rec_batches"),
    }


def _timed_product_run(
    *,
    runtime: str,
    phase: str,
    iteration: int,
    source: dict[str, Any],
    worker: Path,
    candidate_ort: Path,
    rss_interval_seconds: float,
) -> TimedProductRun:
    from sublift.benchmark.srt import load_srt

    with tempfile.TemporaryDirectory(
        prefix=f"sublift-paddle-perf-{runtime}-"
    ) as temp_dir:
        output_path = Path(temp_dir) / "output.srt"
        video_path = REPO_ROOT / str(source["video"])
        cli_command = [
            sys.executable,
            "-m",
            "sublift.cli",
            "extract",
            str(video_path),
            "-o",
            str(output_path),
            "--engine",
            "paddle",
            "--runtime",
            runtime,
            "--fps",
            str(source["fps"]),
            "--confidence",
            str(source["confidence"]),
            "--script",
            str(source["script"]),
        ]
        if platform.system() == "Darwin":
            command = ["/usr/bin/time", "-lp", *cli_command]
        else:
            command = ["/usr/bin/time", "-v", *cli_command]

        env = os.environ.copy()
        env["SUBLIFT_PADDLE_PERF_DIAGNOSTICS"] = "1"
        if runtime == "cpp":
            env["SUBLIFT_WORKER_PATH"] = str(worker)
            library_var = (
                "DYLD_LIBRARY_PATH"
                if platform.system() == "Darwin"
                else "LD_LIBRARY_PATH"
            )
            existing = env.get(library_var)
            library_dir = str(candidate_ort.parent)
            env[library_var] = (
                f"{library_dir}{os.pathsep}{existing}" if existing else library_dir
            )

        start = time.perf_counter()
        proc = subprocess.Popen(
            command,
            cwd=REPO_ROOT,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            start_new_session=True,
        )
        peak_rss_mb = 0.0
        stop_sampling = threading.Event()

        def sample_rss() -> None:
            nonlocal peak_rss_mb
            while not stop_sampling.is_set():
                peak_rss_mb = max(
                    peak_rss_mb,
                    _process_tree_rss_mb(proc.pid),
                )
                stop_sampling.wait(rss_interval_seconds)

        sampler = threading.Thread(target=sample_rss, daemon=True)
        sampler.start()
        timeout = max(300.0, float(source["duration_seconds"]) * 6.0)
        try:
            stdout, stderr = proc.communicate(timeout=timeout)
        except subprocess.TimeoutExpired as exc:
            os.killpg(proc.pid, signal.SIGKILL)
            stdout, stderr = proc.communicate()
            raise RuntimeError(
                f"Paddle {runtime} performance run timed out after {timeout:.0f}s"
            ) from exc
        finally:
            stop_sampling.set()
            sampler.join(timeout=2.0)
        wall_time = time.perf_counter() - start
        if proc.returncode != 0:
            detail = (stderr or stdout)[-3000:].strip()
            raise RuntimeError(
                f"Paddle {runtime} performance run failed: {detail}"
            )
        if not output_path.is_file():
            raise RuntimeError(f"Paddle {runtime} did not create an SRT output")
        if peak_rss_mb <= 0.0:
            raise RuntimeError(f"Paddle {runtime} process-tree RSS was not measured")
        user_time, system_time = _parse_cpu_times(stderr)
        runtime_stats = _parse_runtime_stats(stderr, runtime=runtime)
        return TimedProductRun(
            runtime=runtime,
            phase=phase,
            iteration=iteration,
            wall_time_sec=wall_time,
            user_time_sec=user_time,
            system_time_sec=system_time,
            peak_tree_rss_mb=peak_rss_mb,
            output_sha256=_sha256(output_path),
            output_entries=len(load_srt(output_path)),
            ocr_calls=int(runtime_stats["ocr_calls"] or 0),
            recognize_calls=runtime_stats["recognize_calls"],
            det_boxes=runtime_stats["det_boxes"],
            cls_batches=runtime_stats["cls_batches"],
            rec_batches=runtime_stats["rec_batches"],
        )


def _summarize(runs: list[TimedProductRun]) -> PaddlePerfMetrics:
    if not runs:
        raise RuntimeError("cannot summarize zero Paddle performance runs")
    walls = [run.wall_time_sec for run in runs]
    rss = [run.peak_tree_rss_mb for run in runs]
    hashes = {run.output_sha256 for run in runs}
    entries = {run.output_entries for run in runs}
    ocr_calls = {run.ocr_calls for run in runs}
    if len(hashes) != 1 or len(entries) != 1 or len(ocr_calls) != 1:
        raise RuntimeError("measured Paddle outputs are not deterministic")

    def exact_optional(field_name: str) -> int | None:
        values = {
            getattr(run, field_name)
            for run in runs
            if getattr(run, field_name) is not None
        }
        if not values:
            return None
        if len(values) != 1:
            raise RuntimeError(
                f"measured Paddle {field_name} is not deterministic"
            )
        return int(next(iter(values)))

    return PaddlePerfMetrics(
        wall_time_sec=statistics.median(walls),
        peak_rss_mb=statistics.median(rss),
        user_time_sec=statistics.median(run.user_time_sec for run in runs),
        system_time_sec=statistics.median(run.system_time_sec for run in runs),
        wall_min_sec=min(walls),
        wall_max_sec=max(walls),
        peak_rss_max_mb=max(rss),
        output_sha256=next(iter(hashes)),
        output_entries=next(iter(entries)),
        measured_run_count=len(runs),
        ocr_call_count=next(iter(ocr_calls)),
        det_box_count=exact_optional("det_boxes"),
        cls_batch_count=exact_optional("cls_batches"),
        rec_batch_count=exact_optional("rec_batches"),
    )


def _git_environment() -> tuple[str, bool]:
    commit = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=True,
    ).stdout.strip()
    dirty = bool(
        subprocess.run(
            ["git", "status", "--porcelain"],
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
            check=True,
        ).stdout.strip()
    )
    return commit, dirty


def _write_reports(
    report: PaddlePerfReport,
    report_out: Path | None,
    json_out: Path | None,
) -> None:
    if report_out is not None:
        report_out.parent.mkdir(parents=True, exist_ok=True)
        report_out.write_text(generate_markdown_perf_report(report), encoding="utf-8")
    if json_out is not None:
        json_out.parent.mkdir(parents=True, exist_ok=True)
        json_out.write_text(
            json.dumps(
                {
                    "schema_version": 2,
                    "kind": "paddle_native_performance_gate",
                    "overall_passed": report.overall_passed,
                    "failure": report.failure,
                    "oracle": asdict(report.metrics_oracle),
                    "candidate": asdict(report.metrics_candidate),
                    "gates": [asdict(gate) for gate in report.gate_results],
                    "runs": [asdict(run) for run in report.runs],
                    "environment": report.environment,
                },
                indent=2,
                ensure_ascii=False,
            )
            + "\n",
            encoding="utf-8",
        )


def _failure_report(message: str) -> PaddlePerfReport:
    gate = PerfGateResult(
        passed=False,
        name="live_performance_prerequisites",
        oracle_val=1.0,
        candidate_val=0.0,
        ratio=0.0,
        limit=1.0,
        message=f"FAIL ({message})",
    )
    return PaddlePerfReport(
        overall_passed=False,
        metrics_oracle=PaddlePerfMetrics(),
        metrics_candidate=PaddlePerfMetrics(),
        gate_results=[gate],
        failure=message,
    )


def run_paddle_perf_check(
    check: bool = False,
    report_out: Path | None = None,
    json_out: Path | None = None,
    skip_runtime: bool = False,
    *,
    manifest_path: Path = DEFAULT_MANIFEST,
    candidate_worker: Path | None = None,
    candidate_ort_library: Path | None = None,
    measured_runs: int | None = None,
    warmup_runs: int | None = None,
) -> PaddlePerfReport:
    """Run the real canonical Paddle product benchmark."""
    if skip_runtime:
        report = _failure_report("live runtime execution was explicitly skipped")
        _write_reports(report, report_out, json_out)
        return report

    try:
        manifest = _load_manifest(manifest_path)
        protocol = manifest["protocol"]
        source = manifest["source"]
        gates = manifest["gates"]
        candidate_manifest = manifest["candidate"]
        rounds = (
            int(measured_runs)
            if measured_runs is not None
            else int(protocol["measured_runs_per_runtime"])
        )
        warmups = (
            int(warmup_runs)
            if warmup_runs is not None
            else int(protocol["warmup_runs_per_runtime"])
        )
        if rounds <= 0 or warmups < 0:
            raise RuntimeError("warm-up/measured run counts are invalid")
        if check and rounds < 3:
            raise RuntimeError("--check requires at least three measured runs")

        worker = _resolve_worker(candidate_worker)
        candidate_ort = _resolve_candidate_ort(worker, candidate_ort_library)
        python_ort = _python_ort_library()
        expected_ort_sha = str(candidate_manifest["onnxruntime_sha256"])
        python_ort_sha = _sha256(python_ort)
        candidate_ort_sha = _sha256(candidate_ort)
        if python_ort_sha != expected_ort_sha:
            raise RuntimeError(
                f"Python ORT hash mismatch: {python_ort_sha} != {expected_ort_sha}"
            )
        if candidate_ort_sha != expected_ort_sha:
            raise RuntimeError(
                "Candidate ORT is not the canonical binary: "
                f"{candidate_ort_sha} != {expected_ort_sha}"
            )

        rss_interval = float(protocol["rss_sample_interval_seconds"])
        all_runs: list[TimedProductRun] = []
        expected_output_sha = str(source["expected_srt_sha256"])

        for iteration in range(warmups):
            for runtime in ("python", "cpp"):
                print(
                    f"[Paddle Perf Gate] warm-up {iteration + 1}/{warmups}: "
                    f"{runtime}",
                    flush=True,
                )
                run = _timed_product_run(
                    runtime=runtime,
                    phase="warmup",
                    iteration=iteration,
                    source=source,
                    worker=worker,
                    candidate_ort=candidate_ort,
                    rss_interval_seconds=rss_interval,
                )
                if run.output_sha256 != expected_output_sha:
                    raise RuntimeError(
                        f"{runtime} warm-up output hash mismatch: "
                        f"{run.output_sha256} != {expected_output_sha}"
                    )
                all_runs.append(run)

        measured: list[TimedProductRun] = []
        for iteration in range(rounds):
            order = (
                ("python", "cpp")
                if iteration % 2 == 0
                else ("cpp", "python")
            )
            for runtime in order:
                print(
                    f"[Paddle Perf Gate] measured {iteration + 1}/{rounds}: "
                    f"{runtime}",
                    flush=True,
                )
                run = _timed_product_run(
                    runtime=runtime,
                    phase="measured",
                    iteration=iteration,
                    source=source,
                    worker=worker,
                    candidate_ort=candidate_ort,
                    rss_interval_seconds=rss_interval,
                )
                measured.append(run)
                all_runs.append(run)

        oracle_runs = [run for run in measured if run.runtime == "python"]
        candidate_runs = [run for run in measured if run.runtime == "cpp"]
        oracle = _summarize(oracle_runs)
        candidate = _summarize(candidate_runs)
        quality_passed = (
            oracle.output_sha256 == expected_output_sha
            and candidate.output_sha256 == expected_output_sha
            and oracle.output_sha256 == candidate.output_sha256
        )
        report = evaluate_paddle_perf_hard_gates(
            oracle,
            candidate,
            quality_passed,
            product_wall_limit=float(gates["product_wall_ratio_max"]),
            accepted_wall_limit=float(gates["accepted_wall_ratio_max"]),
            rss_limit=float(gates["rss_ratio_max"]),
        )
        commit, dirty = _git_environment()
        report.runs = all_runs
        report.environment = {
            "manifest": str(manifest_path.resolve()),
            "manifest_sha256": _sha256(manifest_path),
            "git_commit": commit,
            "git_dirty": dirty,
            "platform": platform.platform(),
            "machine": platform.machine(),
            "python_executable": sys.executable,
            "python_ort_library": str(python_ort),
            "python_ort_sha256": python_ort_sha,
            "candidate_worker": str(worker),
            "candidate_worker_sha256": _sha256(worker),
            "candidate_ort_library": str(candidate_ort),
            "candidate_ort_sha256": candidate_ort_sha,
            "candidate_options": candidate_manifest,
            "source": source,
            "protocol": {
                **protocol,
                "actual_warmup_runs_per_runtime": warmups,
                "actual_measured_runs_per_runtime": rounds,
            },
        }
    except Exception as exc:
        report = _failure_report(str(exc))

    _write_reports(report, report_out, json_out)
    return report


def generate_markdown_perf_report(report: PaddlePerfReport) -> str:
    """Format an auditable performance gate report."""
    lines = [
        "# SubLift Phase 6.8 — Paddle Native Performance Gate",
        "",
        f"- **Overall**: {'PASS ✅' if report.overall_passed else 'FAIL ❌'}",
        "- **Primary statistic**: warm-up 后交错运行的 measured median",
        "- **RSS**: CLI 与其 worker 子进程树的同时驻留总和",
        "",
    ]
    if report.failure is not None:
        lines.extend(["## Failure", "", report.failure, ""])

    lines.extend(
        [
            "## Gates",
            "",
            "| Gate | Python | C++ | Ratio | Limit | Status |",
            "|---|---:|---:|---:|---:|---|",
        ]
    )
    for gate in report.gate_results:
        lines.append(
            f"| `{gate.name}` | {gate.oracle_val:.3f} | "
            f"{gate.candidate_val:.3f} | {gate.ratio:.4f}x | "
            f"{gate.limit:.4f}x | {'PASS' if gate.passed else 'FAIL'} |"
        )

    if report.metrics_oracle.measured_run_count > 0:
        py = report.metrics_oracle
        cpp = report.metrics_candidate
        lines.extend(
            [
                "",
                "## Median metrics",
                "",
                "| Runtime | Wall | User CPU | Sys CPU | Tree RSS | "
                "Wall min–max | Entries | Output SHA256 |",
                "|---|---:|---:|---:|---:|---:|---:|---|",
                f"| Python | {py.wall_time_sec:.3f}s | {py.user_time_sec:.3f}s | "
                f"{py.system_time_sec:.3f}s | {py.peak_rss_mb:.1f} MiB | "
                f"{py.wall_min_sec:.3f}–{py.wall_max_sec:.3f}s | "
                f"{py.output_entries} | `{py.output_sha256}` |",
                f"| C++ | {cpp.wall_time_sec:.3f}s | {cpp.user_time_sec:.3f}s | "
                f"{cpp.system_time_sec:.3f}s | {cpp.peak_rss_mb:.1f} MiB | "
                f"{cpp.wall_min_sec:.3f}–{cpp.wall_max_sec:.3f}s | "
                f"{cpp.output_entries} | `{cpp.output_sha256}` |",
                "",
                "## Workload census",
                "",
                f"- Python OCR calls: `{py.ocr_call_count}`",
                f"- C++ OCR calls: `{cpp.ocr_call_count}`",
                f"- C++ Det boxes / Cls batches / Rec batches: "
                f"`{cpp.det_box_count}` / `{cpp.cls_batch_count}` / "
                f"`{cpp.rec_batch_count}`",
                "",
                "## Runs",
                "",
                "| Phase | Iteration | Runtime | Wall | User | Sys | Tree RSS |",
                "|---|---:|---|---:|---:|---:|---:|",
            ]
        )
        for run in report.runs:
            lines.append(
                f"| {run.phase} | {run.iteration + 1} | {run.runtime} | "
                f"{run.wall_time_sec:.3f}s | {run.user_time_sec:.3f}s | "
                f"{run.system_time_sec:.3f}s | "
                f"{run.peak_tree_rss_mb:.1f} MiB |"
            )

        lines.extend(["", "## Environment", ""])
        for key, value in report.environment.items():
            rendered = json.dumps(value, ensure_ascii=False, sort_keys=True)
            lines.append(f"- **{key}**: `{rendered}`")

    return "\n".join(lines) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Real Paddle Native Python/C++ performance gate"
    )
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--skip-runtime", action="store_true")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--candidate-worker", type=Path)
    parser.add_argument("--candidate-ort-library", type=Path)
    parser.add_argument("--measured-runs", type=int)
    parser.add_argument("--warmup-runs", type=int)
    parser.add_argument("--report-out", type=Path)
    parser.add_argument("--json-out", type=Path)
    args = parser.parse_args()

    report = run_paddle_perf_check(
        check=args.check,
        report_out=args.report_out,
        json_out=args.json_out,
        skip_runtime=args.skip_runtime,
        manifest_path=args.manifest,
        candidate_worker=args.candidate_worker,
        candidate_ort_library=args.candidate_ort_library,
        measured_runs=args.measured_runs,
        warmup_runs=args.warmup_runs,
    )
    print(
        f"[Paddle Perf Gate] Status: "
        f"{'PASS' if report.overall_passed else 'FAIL'}"
    )
    if report.failure is not None:
        print(f"[FAIL] {report.failure}", file=sys.stderr)
    if args.check and not report.overall_passed:
        sys.exit(1)


if __name__ == "__main__":
    main()
