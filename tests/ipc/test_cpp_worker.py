"""
tests/ipc/test_cpp_worker.py
-----------------------------
Process-level pytest suite for C++ Worker (sublift_worker).
Launches sublift_worker binary over UDS and validates IPC protocol round-trips,
handshake capabilities, path mode, frame mode, cancellation, and engine rejection.
"""

from __future__ import annotations

import asyncio
import base64
import json
import subprocess
import tempfile
from pathlib import Path
from typing import Any, cast

import pytest


# Prefer Release (cpp-rel) then Debug (cpp), same as product resolve_worker_bin.
def _resolve_worker_bin() -> Path:
    root = Path(__file__).resolve().parents[2]
    for rel in (
        Path("build") / "cpp-rel" / "bin" / "sublift_worker",
        Path("build") / "cpp" / "bin" / "sublift_worker",
    ):
        candidate = root / rel
        if candidate.is_file():
            return candidate
    return root / "build" / "cpp" / "bin" / "sublift_worker"


# 8x6 red JPEG (ffmpeg lavfi). Non-image base64 must fail closed in C++ worker.
MINIMAL_JPEG_B64 = (
    "/9j/4AAQSkZJRgABAgAAAQABAAD//gAQTGF2YzYyLjI4LjEwMAD/2wBDAAgEBAQEBAUFBQUFBQYG"
    "BgYGBgYGBgYGBgYHBwcICAgHBwcGBgcHCAgICAkJCQgICAgJCQoKCgwMCwsODg4RERT/xABMAAEB"
    "AAAAAAAAAAAAAAAAAAAABgEBAQAAAAAAAAAAAAAAAAAABgcQAQAAAAAAAAAAAAAAAAAAAAARAQ"
    "AAAAAAAAAAAAAAAAAAAAD/wAARCAAGAAgDASIAAhEAAxEA/9oADAMBAAIRAxEAPwCLAE1/f//Z"
)
assert base64.b64decode(MINIMAL_JPEG_B64)[:2] == b"\xff\xd8"


async def send_framed_msg(writer: asyncio.StreamWriter, data: dict[str, Any]) -> None:
    raw_json = json.dumps(data).encode("utf-8")
    length_prefix = len(raw_json).to_bytes(4, byteorder="big")
    writer.write(length_prefix + raw_json)
    await writer.drain()


async def read_framed_msg(reader: asyncio.StreamReader) -> dict[str, Any]:
    prefix = await reader.readexactly(4)
    length = int.from_bytes(prefix, byteorder="big")
    payload = await reader.readexactly(length)
    res = json.loads(payload.decode("utf-8"))
    return cast(dict[str, Any], res)


async def generate_synthetic_video(tmp_dir: Path, duration_s: int = 3) -> Path:
    """Generate a portable fixture and fail at its actual point of failure.

    The mock Worker test does not need burned-in text.  ``testsrc`` is part of
    ffmpeg's core lavfi filters, unlike ``drawtext`` which depends on optional
    freetype/libass builds.
    """
    video_path = tmp_dir / "ipc_pytest_video.mp4"
    cmd = [
        "ffmpeg",
        "-y",
        "-f",
        "lavfi",
        "-i",
        f"testsrc=size=320x240:rate=2:duration={duration_s}",
        "-c:v",
        "libx264",
        "-pix_fmt",
        "yuv420p",
        str(video_path),
    ]
    proc = await asyncio.create_subprocess_exec(
        *cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE
    )
    _stdout, stderr = await proc.communicate()
    if proc.returncode != 0 or not video_path.is_file():
        detail = stderr.decode("utf-8", errors="replace")[-2000:]
        raise RuntimeError(f"ffmpeg synthetic fixture generation failed: {detail}")
    return video_path


async def connect_unix_with_retry(
    socket_path: Path, timeout: float = 3.0
) -> tuple[asyncio.StreamReader, asyncio.StreamWriter]:
    start = asyncio.get_event_loop().time()
    while True:
        try:
            return await asyncio.open_unix_connection(str(socket_path))
        except (ConnectionRefusedError, FileNotFoundError):
            if asyncio.get_event_loop().time() - start > timeout:
                raise
            await asyncio.sleep(0.1)


def check_worker_bin() -> str:
    worker = _resolve_worker_bin()
    if not worker.exists():
        pytest.skip("sublift_worker binary not compiled (build/cpp-rel or build/cpp)")
    return str(worker)


def test_cpp_worker_handshake() -> None:
    worker_bin = check_worker_bin()

    async def run() -> None:
        with tempfile.TemporaryDirectory() as tmp_dir_str:
            socket_path = Path(tmp_dir_str) / "worker.sock"
            proc = await asyncio.create_subprocess_exec(
                worker_bin,
                "--socket",
                str(socket_path),
                "--engine",
                "mock",
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )

            try:
                reader, writer = await connect_unix_with_retry(socket_path)
                await send_framed_msg(
                    writer, {"type": "hello", "client": "pytest", "protocol_version": 1}
                )
                bye = await read_framed_msg(reader)

                assert bye["type"] == "bye"
                assert bye["runtime"] == "cpp"
                assert "mock" in bye["engines"]
                assert "paddle" not in bye["engines"]

                writer.close()
                await writer.wait_closed()
            finally:
                try:
                    proc.terminate()
                    await proc.wait()
                except Exception:
                    pass

    asyncio.run(run())


def test_cpp_worker_rejects_overlong_socket_path() -> None:
    """AF_UNIX path truncation must fail explicitly before bind/unlink."""
    worker_bin = check_worker_bin()

    async def run() -> None:
        with tempfile.TemporaryDirectory() as tmp_dir_str:
            socket_path = Path(tmp_dir_str) / ("x" * 200)
            proc = await asyncio.create_subprocess_exec(
                worker_bin,
                "--socket",
                str(socket_path),
                "--engine",
                "mock",
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
            )
            _stdout, stderr = await proc.communicate()
            assert proc.returncode == 1
            assert b"path is too long" in stderr

    asyncio.run(run())


def test_cpp_worker_path_mode() -> None:
    worker_bin = check_worker_bin()

    async def run() -> None:
        with tempfile.TemporaryDirectory() as tmp_dir_str:
            tmp_dir = Path(tmp_dir_str)
            socket_path = tmp_dir / "worker.sock"
            video_path = await generate_synthetic_video(tmp_dir, duration_s=30)

            proc = await asyncio.create_subprocess_exec(
                worker_bin,
                "--socket",
                str(socket_path),
                "--engine",
                "mock",
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )

            try:
                reader, writer = await connect_unix_with_retry(socket_path)
                await send_framed_msg(
                    writer,
                    {
                        "type": "start_job",
                        "video_id": "v_pytest_path",
                        "fps": 1.0,
                        "engine": "mock",
                        "confidence_threshold": 0.5,
                        "video_path": str(video_path),
                    },
                )

                done_received = False
                entries_received = False

                while not done_received:
                    msg = await read_framed_msg(reader)
                    if msg["type"] == "entries":
                        entries_received = True
                        assert msg["video_id"] == "v_pytest_path"
                    elif msg["type"] == "done":
                        done_received = True
                        assert msg["ok"] is True

                assert entries_received
                assert done_received

                writer.close()
                await writer.wait_closed()
            finally:
                try:
                    proc.terminate()
                    await proc.wait()
                except Exception:
                    pass

    asyncio.run(run())


def test_cpp_worker_cancel() -> None:
    worker_bin = check_worker_bin()

    async def run() -> None:
        with tempfile.TemporaryDirectory() as tmp_dir_str:
            tmp_dir = Path(tmp_dir_str)
            socket_path = tmp_dir / "worker.sock"
            video_path = await generate_synthetic_video(tmp_dir)

            proc = await asyncio.create_subprocess_exec(
                worker_bin,
                "--socket",
                str(socket_path),
                "--engine",
                "mock",
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )

            try:
                reader, writer = await connect_unix_with_retry(socket_path)
                await send_framed_msg(
                    writer,
                    {
                        "type": "start_job",
                        "video_id": "v_pytest_cancel",
                        "fps": 1.0,
                        "engine": "mock",
                        "confidence_threshold": 0.5,
                        "video_path": str(video_path),
                    },
                )

                ready = await read_framed_msg(reader)
                assert ready["type"] == "progress"
                assert ready["stage"] == "ready"

                await send_framed_msg(writer, {"type": "cancel_job", "video_id": "v_pytest_cancel"})

                msg = await read_framed_msg(reader)
                if msg["type"] == "progress":
                    msg = await read_framed_msg(reader)

                assert msg["type"] == "done"
                assert msg["ok"] is False
                assert msg["error"] == "cancelled"

                writer.close()
                await writer.wait_closed()
            finally:
                try:
                    proc.terminate()
                    await proc.wait()
                except Exception:
                    pass

    asyncio.run(run())


def test_cpp_worker_frame_mode() -> None:
    worker_bin = check_worker_bin()

    async def run() -> None:
        with tempfile.TemporaryDirectory() as tmp_dir_str:
            socket_path = Path(tmp_dir_str) / "worker.sock"
            proc = await asyncio.create_subprocess_exec(
                worker_bin,
                "--socket",
                str(socket_path),
                "--engine",
                "mock",
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )

            try:
                reader, writer = await connect_unix_with_retry(socket_path)
                await send_framed_msg(
                    writer,
                    {
                        "type": "start_job",
                        "video_id": "v_pytest_frame",
                        "fps": 1.0,
                        "engine": "mock",
                        "confidence_threshold": 0.5,
                    },
                )

                ready_msg = await read_framed_msg(reader)
                assert ready_msg["type"] == "progress"
                assert ready_msg["stage"] == "ready"

                await send_framed_msg(
                    writer,
                    {
                        "type": "frame",
                        "video_id": "v_pytest_frame",
                        "ts_ms": 0,
                        "jpeg_bytes": MINIMAL_JPEG_B64,
                    },
                )
                frame_resp = await read_framed_msg(reader)
                assert frame_resp["type"] == "progress", frame_resp
                assert frame_resp.get("stage") == "processing"

                await send_framed_msg(writer, {"type": "finalize", "video_id": "v_pytest_frame"})

                saw_entries = False
                saw_done = False
                while not saw_done:
                    msg = await read_framed_msg(reader)
                    if msg["type"] == "entries":
                        saw_entries = True
                        assert msg.get("is_final") is True
                    elif msg["type"] == "done":
                        saw_done = True
                        assert msg["ok"] is True
                    elif msg["type"] == "error":
                        raise AssertionError(f"unexpected error: {msg}")
                assert saw_entries

                writer.close()
                await writer.wait_closed()
            finally:
                try:
                    proc.terminate()
                    await proc.wait()
                except Exception:
                    pass

    asyncio.run(run())


def test_cpp_worker_client_bye_closes_connection() -> None:
    """A client ``bye`` is a close request, not a second handshake."""
    worker_bin = check_worker_bin()

    async def run() -> None:
        with tempfile.TemporaryDirectory() as tmp_dir_str:
            socket_path = Path(tmp_dir_str) / "worker.sock"
            proc = await asyncio.create_subprocess_exec(
                worker_bin, "--socket", str(socket_path), "--engine", "mock",
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            )
            try:
                reader, writer = await connect_unix_with_retry(socket_path)
                await send_framed_msg(
                    writer, {"type": "hello", "client": "pytest", "protocol_version": 1}
                )
                hello = await read_framed_msg(reader)
                assert hello["type"] == "bye"
                assert hello["engines"] == ["mock"]

                await send_framed_msg(writer, {"type": "bye"})
                assert await asyncio.wait_for(reader.read(), timeout=1.0) == b""
                writer.close()
                await writer.wait_closed()
            finally:
                proc.terminate()
                await proc.wait()

    asyncio.run(run())


def test_cpp_worker_frame_mode_rejects_oversized_base64() -> None:
    """Encoded JPEG data is bounded before base64 decoding/allocation."""
    worker_bin = check_worker_bin()
    oversized_b64 = "A" * (28 * 1024 * 1024)

    async def run() -> None:
        with tempfile.TemporaryDirectory() as tmp_dir_str:
            socket_path = Path(tmp_dir_str) / "worker.sock"
            proc = await asyncio.create_subprocess_exec(
                worker_bin, "--socket", str(socket_path), "--engine", "mock",
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            )
            try:
                reader, writer = await connect_unix_with_retry(socket_path)
                await send_framed_msg(
                    writer,
                    {
                        "type": "start_job",
                        "video_id": "v_oversized_frame",
                        "fps": 1.0,
                        "engine": "mock",
                        "confidence_threshold": 0.5,
                    },
                )
                assert (await read_framed_msg(reader))["stage"] == "ready"
                await send_framed_msg(
                    writer,
                    {
                        "type": "frame",
                        "video_id": "v_oversized_frame",
                        "ts_ms": 0,
                        "jpeg_bytes": oversized_b64,
                    },
                )
                error = await read_framed_msg(reader)
                assert error["type"] == "error"
                assert "JPEG base64 过大" in error["message"]
                writer.close()
                await writer.wait_closed()
            finally:
                proc.terminate()
                await proc.wait()

    asyncio.run(run())


def test_cpp_worker_engine_mismatch() -> None:
    worker_bin = check_worker_bin()

    async def run() -> None:
        with tempfile.TemporaryDirectory() as tmp_dir_str:
            socket_path = Path(tmp_dir_str) / "worker.sock"
            proc = await asyncio.create_subprocess_exec(
                worker_bin,
                "--socket",
                str(socket_path),
                "--engine",
                "mock",
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )

            try:
                reader, writer = await connect_unix_with_retry(socket_path)
                await send_framed_msg(
                    writer,
                    {
                        "type": "start_job",
                        "video_id": "v_mismatch",
                        "fps": 1.0,
                        "engine": "vision",
                        "confidence_threshold": 0.5,
                        "video_path": "/tmp/fake.mp4",
                    },
                )

                done = await read_framed_msg(reader)
                assert done["type"] == "done"
                assert done["ok"] is False
                assert "engine 不匹配" in done["error"]

                writer.close()
                await writer.wait_closed()
            finally:
                try:
                    proc.terminate()
                    await proc.wait()
                except Exception:
                    pass

    asyncio.run(run())


def test_cpp_worker_paddle_rejected() -> None:
    worker_bin = check_worker_bin()

    async def run() -> None:
        with tempfile.TemporaryDirectory() as tmp_dir_str:
            socket_path = Path(tmp_dir_str) / "worker.sock"
            proc = await asyncio.create_subprocess_exec(
                worker_bin,
                "--socket",
                str(socket_path),
                "--engine",
                "mock",
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )

            try:
                reader, writer = await connect_unix_with_retry(socket_path)
                await send_framed_msg(
                    writer,
                    {
                        "type": "start_job",
                        "video_id": "v_paddle",
                        "fps": 1.0,
                        "engine": "paddle",
                        "confidence_threshold": 0.5,
                        "video_path": "/tmp/fake.mp4",
                    },
                )

                done = await read_framed_msg(reader)
                assert done["type"] == "done"
                assert done["ok"] is False
                assert "paddle" in done["error"]

                writer.close()
                await writer.wait_closed()
            finally:
                try:
                    proc.terminate()
                    await proc.wait()
                except Exception:
                    pass

    asyncio.run(run())
