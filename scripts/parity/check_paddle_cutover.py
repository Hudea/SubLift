"""Final Paddle C++ product-cutover acceptance gate (feat-06807).

This gate verifies behavior that the operator/quality/performance gates do not:

* the product default really resolves Paddle to the accepted C++ Worker;
* forced Python remains a byte-exact rollback path;
* a default C++ restart returns to the same output without cross-runtime state;
* one continuous source-duration flow is at least ten minutes;
* the real Paddle Worker can cancel an in-flight path job and restart promptly.

The gate never changes OCR engine during fallback and fails closed on missing
assets, model/Worker capability, ORT identity, runtime markers, or output.
"""

from __future__ import annotations

import argparse
import asyncio
import hashlib
import json
import os
import platform
import subprocess
import sys
import tempfile
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any, cast

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from sublift.benchmark.srt import load_srt  # noqa: E402
from sublift.runtime import (  # noqa: E402
    probe_cpp_paddle_available,
    resolve_runtime,
)
from sublift.worker_bin import resolve_worker_bin  # noqa: E402

QUALITY_MANIFEST = REPO_ROOT / "benchmark/datasets/paddle_quality/manifest.v1.json"
PERF_MANIFEST = REPO_ROOT / "benchmark/datasets/paddle_performance/manifest.v1.json"
DEFAULT_REPORT = Path("/tmp/sublift_paddle_cutover.md")
ROLLBACK_SOURCE_ID = "synthetic_mixed_multiline"
LONG_FLOW_SECONDS_MIN = 600.0
CANCEL_SECONDS_MAX = 1.0
RESTART_SECONDS_MAX = 5.0


@dataclass(frozen=True)
class ProductRun:
    name: str
    expected_runtime: str
    wall_seconds: float
    output_sha256: str
    output_entries: int
    runtime_marker: str


@dataclass(frozen=True)
class IpcLifecycle:
    cancel_seconds: float
    restart_seconds: float
    cancel_after_progress_pct: float
    restart_terminal_type: str


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            digest.update(chunk)
    return f"sha256:{digest.hexdigest()}"


def _load_json(path: Path) -> dict[str, Any]:
    if not path.is_file():
        raise RuntimeError(f"required manifest is missing: {path}")
    return cast(dict[str, Any], json.loads(path.read_text(encoding="utf-8")))


def _resolve_worker(explicit: Path | None) -> Path:
    worker = explicit or resolve_worker_bin(REPO_ROOT)
    if worker is None or not worker.is_file() or not os.access(worker, os.X_OK):
        raise RuntimeError("Release sublift_worker is missing or not executable")
    return worker.resolve()


def _resolve_official_ort(explicit: Path | None, expected_sha: str) -> Path:
    candidates = [
        explicit,
        REPO_ROOT
        / ".venv/lib/python3.12/site-packages/onnxruntime/capi/libonnxruntime.1.dylib",
        REPO_ROOT
        / ".venv/lib/python3.12/site-packages/onnxruntime/capi/libonnxruntime.1.28.0.dylib",
    ]
    for candidate in candidates:
        if candidate is None or not candidate.is_file():
            continue
        resolved = candidate.resolve()
        actual = _sha256(resolved)
        if actual != expected_sha:
            raise RuntimeError(
                f"ORT hash mismatch for {resolved}: expected {expected_sha}, got {actual}"
            )
        return resolved
    raise RuntimeError("accepted official ONNX Runtime dylib is missing")


def _verify_worker_ort_binding(worker: Path, ort: Path) -> Path:
    if platform.system() != "Darwin":
        return ort
    linked = subprocess.run(
        ["otool", "-L", str(worker)],
        capture_output=True,
        text=True,
        check=True,
    ).stdout
    load_commands = subprocess.run(
        ["otool", "-l", str(worker)],
        capture_output=True,
        text=True,
        check=True,
    ).stdout
    if "libonnxruntime" not in linked:
        raise RuntimeError("Release Worker is not linked to ONNX Runtime")
    bundled = worker.parent.parent / "lib/libonnxruntime.1.dylib"
    if bundled.is_file() and "@loader_path/../lib" in load_commands:
        if _sha256(bundled.resolve()) != _sha256(ort):
            raise RuntimeError("bundled Worker ORT differs from the accepted official ORT")
        return bundled.resolve()
    if str(ort.parent) in load_commands:
        return ort
    raise RuntimeError(
        "Release Worker has neither a relocatable bundled ORT nor an accepted ORT rpath"
    )


def _source_from_manifest(manifest: dict[str, Any], source_id: str) -> dict[str, Any]:
    for source in manifest.get("sources", []):
        if source.get("id") == source_id:
            return cast(dict[str, Any], source)
    raise RuntimeError(f"quality source not found: {source_id}")


def _verify_source(source: dict[str, Any]) -> Path:
    path = REPO_ROOT / str(source["video"])
    if not path.is_file():
        raise RuntimeError(f"source video is missing: {path}")
    actual = _sha256(path)
    expected = str(source["video_sha256"])
    if actual != expected:
        raise RuntimeError(
            f"source hash mismatch for {path}: expected {expected}, got {actual}"
        )
    return path


def _probe_duration(path: Path) -> float:
    proc = subprocess.run(
        [
            "ffprobe",
            "-v",
            "error",
            "-show_entries",
            "format=duration",
            "-of",
            "default=nw=1:nk=1",
            str(path),
        ],
        capture_output=True,
        text=True,
        check=True,
    )
    return float(proc.stdout.strip())


def _make_long_flow(source: Path, destination: Path) -> float:
    subprocess.run(
        [
            "ffmpeg",
            "-v",
            "error",
            "-y",
            "-stream_loop",
            "3",
            "-i",
            str(source),
            "-t",
            "720",
            "-map",
            "0:v:0",
            "-c",
            "copy",
            str(destination),
        ],
        check=True,
    )
    duration = _probe_duration(destination)
    if duration < LONG_FLOW_SECONDS_MIN:
        raise RuntimeError(
            f"generated long-flow source is only {duration:.3f}s; require >=600s"
        )
    return duration


def _extract_marker(stderr: str, runtime: str) -> str:
    lines = [
        line.strip()
        for line in stderr.splitlines()
        if "engine=paddle" in line and f"runtime={runtime}" in line
    ]
    if not lines:
        raise RuntimeError(
            f"product run did not disclose engine=paddle runtime={runtime}"
        )
    marker = lines[-1]
    if runtime == "cpp" and "status=stable" not in marker:
        raise RuntimeError(f"C++ Paddle is not marked stable: {marker}")
    if "experimental" in marker.lower():
        raise RuntimeError(f"experimental marker remains after cutover: {marker}")
    return marker


def _run_product(
    *,
    name: str,
    video: Path,
    output: Path,
    worker: Path,
    expected_runtime: str,
    requested_runtime: str | None,
    fps: float,
    script: str,
) -> ProductRun:
    command = [
        sys.executable,
        "-m",
        "sublift.cli",
        "extract",
        str(video),
        "-o",
        str(output),
        "--engine",
        "paddle",
        "--fps",
        str(fps),
        "--script",
        script,
    ]
    if requested_runtime is not None:
        command.extend(["--runtime", requested_runtime])

    env = os.environ.copy()
    env["SUBLIFT_WORKER_PATH"] = str(worker)
    started = time.perf_counter()
    proc = subprocess.run(
        command,
        cwd=REPO_ROOT,
        env=env,
        capture_output=True,
        text=True,
        timeout=max(300.0, _probe_duration(video) * 4.0),
    )
    elapsed = time.perf_counter() - started
    if proc.returncode != 0:
        detail = (proc.stderr or proc.stdout)[-4000:].strip()
        raise RuntimeError(f"{name} failed: {detail}")
    if not output.is_file():
        raise RuntimeError(f"{name} did not create an SRT output")
    marker = _extract_marker(proc.stderr, expected_runtime)
    entries = load_srt(output)
    return ProductRun(
        name=name,
        expected_runtime=expected_runtime,
        wall_seconds=elapsed,
        output_sha256=_sha256(output),
        output_entries=len(entries),
        runtime_marker=marker,
    )


async def _connect(socket_path: Path) -> tuple[asyncio.StreamReader, asyncio.StreamWriter]:
    deadline = time.monotonic() + 5.0
    while True:
        try:
            return await asyncio.open_unix_connection(str(socket_path))
        except (ConnectionRefusedError, FileNotFoundError):
            if time.monotonic() >= deadline:
                raise RuntimeError(
                    f"timed out connecting to Worker socket {socket_path}"
                ) from None
            await asyncio.sleep(0.02)


async def _send(writer: asyncio.StreamWriter, payload: dict[str, Any]) -> None:
    raw = json.dumps(payload).encode("utf-8")
    writer.write(len(raw).to_bytes(4, "big") + raw)
    await writer.drain()


async def _recv(
    reader: asyncio.StreamReader,
    *,
    timeout: float = 30.0,
) -> dict[str, Any]:
    async def receive() -> dict[str, Any]:
        prefix = await reader.readexactly(4)
        size = int.from_bytes(prefix, "big")
        body = await reader.readexactly(size)
        return cast(dict[str, Any], json.loads(body.decode("utf-8")))

    return await asyncio.wait_for(receive(), timeout=timeout)


async def _drain_terminal(
    reader: asyncio.StreamReader,
    *,
    timeout: float = 300.0,
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise RuntimeError("timed out waiting for Paddle job terminal message")
        message = await _recv(reader, timeout=remaining)
        if message.get("type") in {"entries", "done", "error"}:
            if message.get("type") == "entries":
                continue
            return message


async def _run_paddle_lifecycle(
    worker: Path,
    long_video: Path,
    restart_video: Path,
) -> IpcLifecycle:
    with tempfile.TemporaryDirectory(prefix="sublift-paddle-cutover-ipc-") as tmp:
        socket_path = Path(tmp) / "worker.sock"
        process = await asyncio.create_subprocess_exec(
            str(worker),
            "--socket",
            str(socket_path),
            "--engine",
            "paddle",
            stdout=asyncio.subprocess.DEVNULL,
            stderr=asyncio.subprocess.PIPE,
        )
        writer: asyncio.StreamWriter | None = None
        try:
            reader, writer = await _connect(socket_path)
            await _send(
                writer,
                {"type": "hello", "client": "paddle_cutover_gate", "protocol_version": 1},
            )
            bye = await _recv(reader)
            if (
                bye.get("type") != "bye"
                or bye.get("runtime") != "cpp"
                or "paddle" not in bye.get("engines", [])
            ):
                raise RuntimeError(f"Worker Paddle capability mismatch: {bye}")

            await _send(
                writer,
                {
                    "type": "start_job",
                    "video_id": "paddle-cancel",
                    "fps": 5.0,
                    "engine": "paddle",
                    "confidence_threshold": 0.5,
                    "video_path": str(long_video),
                    "subtitle_profile": {
                        "script": "auto",
                        "center_x": 0,
                        "center_y": 0,
                        "height": 0,
                        "y_min": 0,
                        "y_max": 0,
                    },
                },
            )
            progress_pct = 0.0
            while progress_pct <= 0.0:
                message = await _recv(reader, timeout=60.0)
                if message.get("type") == "done" and not message.get("ok"):
                    raise RuntimeError(f"Paddle cancel job failed before progress: {message}")
                if message.get("type") == "progress" and message.get("stage") == "processing":
                    progress_pct = float(message.get("pct", 0.0))

            cancel_started = time.perf_counter()
            await _send(
                writer,
                {"type": "cancel_job", "video_id": "paddle-cancel"},
            )
            cancelled = await _drain_terminal(reader, timeout=30.0)
            cancel_seconds = time.perf_counter() - cancel_started
            if cancelled.get("type") != "done" or cancelled.get("error") != "cancelled":
                raise RuntimeError(f"unexpected Paddle cancel terminal: {cancelled}")

            restart_started = time.perf_counter()
            await _send(
                writer,
                {
                    "type": "start_job",
                    "video_id": "paddle-restart",
                    "fps": 2.0,
                    "engine": "paddle",
                    "confidence_threshold": 0.5,
                    "video_path": str(restart_video),
                    "subtitle_profile": {
                        "script": "auto",
                        "center_x": 0,
                        "center_y": 0,
                        "height": 0,
                        "y_min": 0,
                        "y_max": 0,
                    },
                },
            )
            first = await _recv(reader, timeout=30.0)
            restart_seconds = time.perf_counter() - restart_started
            if first.get("type") not in {"progress", "push_entry", "entries", "done"}:
                raise RuntimeError(f"unexpected first Paddle restart response: {first}")
            terminal = first
            if first.get("type") not in {"done", "error"}:
                terminal = await _drain_terminal(reader, timeout=300.0)
            if terminal.get("type") != "done" or terminal.get("ok") is not True:
                raise RuntimeError(f"Paddle restart failed: {terminal}")

            return IpcLifecycle(
                cancel_seconds=cancel_seconds,
                restart_seconds=restart_seconds,
                cancel_after_progress_pct=progress_pct,
                restart_terminal_type=str(terminal.get("type")),
            )
        finally:
            if writer is not None:
                writer.close()
                await writer.wait_closed()
            if process.returncode is None:
                process.terminate()
                try:
                    await asyncio.wait_for(process.wait(), timeout=2.0)
                except TimeoutError:
                    process.kill()
                    await process.wait()


def _render_markdown(report: dict[str, Any]) -> str:
    runs = report["product_runs"]
    lifecycle = report["ipc_lifecycle"]
    lines = [
        "# Phase 6.8 Paddle C++ Product Cutover Acceptance",
        "",
        f"- **Overall:** {'PASS' if report['overall_passed'] else 'FAIL'}",
        f"- **Worker:** `{report['environment']['worker']}`",
        f"- **Worker SHA256:** `{report['environment']['worker_sha256']}`",
        f"- **ORT SHA256:** `{report['environment']['ort_sha256']}`",
        f"- **Long source duration:** {report['long_flow']['duration_seconds']:.3f}s",
        "",
        "## Product routing / rollback",
        "",
        "| Run | Runtime | Wall | Entries | SRT SHA256 |",
        "|---|---:|---:|---:|---|",
    ]
    for run in runs:
        lines.append(
            f"| {run['name']} | {run['expected_runtime']} | "
            f"{run['wall_seconds']:.3f}s | {run['output_entries']} | "
            f"`{run['output_sha256']}` |"
        )
    lines.extend(
        [
            "",
            "Default C++ → forced Python → default C++ restart produced byte-exact SRT.",
            "",
            "## Long flow / lifecycle",
            "",
            f"- Default C++ long flow: {report['long_flow']['duration_seconds']:.3f}s source, "
            f"{report['long_flow']['wall_seconds']:.3f}s wall, "
            f"{report['long_flow']['output_entries']} entries.",
            f"- In-flight Paddle cancel: {lifecycle['cancel_seconds'] * 1000:.1f}ms "
            f"(gate ≤ {CANCEL_SECONDS_MAX * 1000:.0f}ms), sent after "
            f"progress={lifecycle['cancel_after_progress_pct']:.6f}.",
            f"- Same-Worker restart readiness: {lifecycle['restart_seconds'] * 1000:.1f}ms "
            f"(gate ≤ {RESTART_SECONDS_MAX * 1000:.0f}ms).",
            "",
            "Python Paddle remains available through explicit `--runtime python` / "
            "`SUBLIFT_RUNTIME=python`; no fallback changes the requested OCR engine.",
        ]
    )
    if report.get("failure"):
        lines.extend(["", "## Failure", "", f"`{report['failure']}`"])
    return "\n".join(lines) + "\n"


def run_cutover_gate(
    *,
    worker_path: Path | None = None,
    ort_path: Path | None = None,
) -> dict[str, Any]:
    quality = _load_json(QUALITY_MANIFEST)
    perf = _load_json(PERF_MANIFEST)
    worker = _resolve_worker(worker_path)
    expected_ort_sha = str(perf["candidate"]["onnxruntime_sha256"])
    ort = _resolve_official_ort(ort_path, expected_ort_sha)
    runtime_ort = _verify_worker_ort_binding(worker, ort)

    probe_env = os.environ.copy()
    probe_env["SUBLIFT_WORKER_PATH"] = str(worker)
    if not probe_cpp_paddle_available(env_override=probe_env, repo_root=REPO_ROOT):
        raise RuntimeError("accepted Release Worker did not pass Paddle capability probe")
    default_choice = resolve_runtime(
        requested_engine="paddle",
        env_override={},
        cpp_paddle_available=True,
    )
    rollback_choice = resolve_runtime(
        requested_runtime="python",
        requested_engine="paddle",
        env_override={},
        cpp_paddle_available=True,
    )
    if default_choice.runtime != "cpp" or default_choice.engine != "paddle":
        raise RuntimeError(f"product default route is not C++ Paddle: {default_choice}")
    if rollback_choice.runtime != "python" or rollback_choice.engine != "paddle":
        raise RuntimeError(f"Python rollback changed runtime/engine: {rollback_choice}")

    source = _source_from_manifest(quality, ROLLBACK_SOURCE_ID)
    source_path = _verify_source(source)
    with tempfile.TemporaryDirectory(prefix="sublift-paddle-cutover-") as tmp:
        temp = Path(tmp)
        long_video = temp / "mixed-long-720s.mp4"
        long_duration = _make_long_flow(source_path, long_video)

        runs = [
            _run_product(
                name="default_cpp_before_rollback",
                video=source_path,
                output=temp / "default-before.srt",
                worker=worker,
                expected_runtime="cpp",
                requested_runtime=None,
                fps=float(source["fps"]),
                script=str(source["script"]),
            ),
            _run_product(
                name="forced_python_rollback",
                video=source_path,
                output=temp / "python-rollback.srt",
                worker=worker,
                expected_runtime="python",
                requested_runtime="python",
                fps=float(source["fps"]),
                script=str(source["script"]),
            ),
            _run_product(
                name="default_cpp_after_restart",
                video=source_path,
                output=temp / "default-after.srt",
                worker=worker,
                expected_runtime="cpp",
                requested_runtime=None,
                fps=float(source["fps"]),
                script=str(source["script"]),
            ),
        ]
        hashes = {run.output_sha256 for run in runs}
        if len(hashes) != 1:
            raise RuntimeError(
                "default C++ → forced Python → default C++ outputs are not byte-exact"
            )
        if min(run.output_entries for run in runs) <= 0:
            raise RuntimeError("rollback exercise produced an empty SRT")

        long_run = _run_product(
            name="default_cpp_long_flow",
            video=long_video,
            output=temp / "long-flow.srt",
            worker=worker,
            expected_runtime="cpp",
            requested_runtime=None,
            fps=float(source["fps"]),
            script=str(source["script"]),
        )
        if long_run.output_entries <= 0:
            raise RuntimeError(">=10min C++ Paddle long flow produced no subtitles")

        lifecycle = asyncio.run(
            _run_paddle_lifecycle(worker, long_video, source_path)
        )
        if lifecycle.cancel_seconds > CANCEL_SECONDS_MAX:
            raise RuntimeError(
                f"Paddle cancel {lifecycle.cancel_seconds:.3f}s exceeds 1s"
            )
        if lifecycle.restart_seconds > RESTART_SECONDS_MAX:
            raise RuntimeError(
                f"Paddle restart {lifecycle.restart_seconds:.3f}s exceeds 5s"
            )

        return {
            "schema_version": 1,
            "kind": "paddle_cpp_product_cutover_acceptance",
            "overall_passed": True,
            "environment": {
                "worker": str(worker),
                "worker_sha256": _sha256(worker),
                "ort": str(runtime_ort),
                "ort_sha256": _sha256(runtime_ort),
                "model": "PP-OCRv6-small",
            },
            "routes": {
                "product_default": {
                    "runtime": default_choice.runtime,
                    "engine": default_choice.engine,
                    "resolved_via": default_choice.resolved_via.value,
                },
                "forced_python": {
                    "runtime": rollback_choice.runtime,
                    "engine": rollback_choice.engine,
                    "resolved_via": rollback_choice.resolved_via.value,
                },
            },
            "product_runs": [asdict(run) for run in runs],
            "rollback_output_sha256_exact": True,
            "long_flow": {
                "duration_seconds": long_duration,
                "wall_seconds": long_run.wall_seconds,
                "output_entries": long_run.output_entries,
                "output_sha256": long_run.output_sha256,
                "runtime_marker": long_run.runtime_marker,
            },
            "ipc_lifecycle": asdict(lifecycle),
            "failure": None,
        }


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="fail non-zero when any gate fails")
    parser.add_argument("--worker", type=Path, help="accepted Release sublift_worker")
    parser.add_argument("--ort-library", type=Path, help="accepted official ORT dylib")
    parser.add_argument("--report-out", type=Path, default=DEFAULT_REPORT)
    parser.add_argument("--json-out", type=Path)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    try:
        report = run_cutover_gate(
            worker_path=args.worker,
            ort_path=args.ort_library,
        )
    except Exception as exc:
        report = {
            "schema_version": 1,
            "kind": "paddle_cpp_product_cutover_acceptance",
            "overall_passed": False,
            "environment": {},
            "product_runs": [],
            "long_flow": {},
            "ipc_lifecycle": {},
            "failure": str(exc),
        }

    markdown = _render_markdown(report) if report["overall_passed"] else (
        "# Phase 6.8 Paddle C++ Product Cutover Acceptance\n\n"
        f"- **Overall:** FAIL\n- **Failure:** `{report['failure']}`\n"
    )
    args.report_out.parent.mkdir(parents=True, exist_ok=True)
    args.report_out.write_text(markdown, encoding="utf-8")
    if args.json_out is not None:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(
            json.dumps(report, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
    print(markdown, end="")
    return 1 if args.check and not report["overall_passed"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
