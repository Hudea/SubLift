#!/usr/bin/env python3
"""
scripts/parity/dump_ipc_session.py
-----------------------------------
Dump or verify the C++ Worker IPC session golden sequence against frozen goldens.

Usage:
  python scripts/parity/dump_ipc_session.py [--check]
"""

from __future__ import annotations

import argparse
import asyncio
import json
import subprocess
import sys
import tempfile
from pathlib import Path

GOLDEN_PATH = (
    Path(__file__).resolve().parents[2]
    / "benchmark"
    / "parity"
    / "goldens"
    / "ipc"
    / "path_mock_session.v1.json"
)
WORKER_BIN = Path(__file__).resolve().parents[2] / "build" / "cpp" / "bin" / "sublift_worker"


async def send_framed_msg(writer: asyncio.StreamWriter, data: dict) -> None:
    raw_json = json.dumps(data).encode("utf-8")
    length_prefix = len(raw_json).to_bytes(4, byteorder="big")
    writer.write(length_prefix + raw_json)
    await writer.drain()


async def read_framed_msg(reader: asyncio.StreamReader) -> dict:
    prefix = await reader.readexactly(4)
    length = int.from_bytes(prefix, byteorder="big")
    payload = await reader.readexactly(length)
    return json.loads(payload.decode("utf-8"))


async def generate_synthetic_video(tmp_dir: Path) -> Path:
    video_path = tmp_dir / "ipc_synthetic_test.mp4"
    cmd = [
        "ffmpeg",
        "-y",
        "-f",
        "lavfi",
        "-i",
        "testsrc=size=320x240:rate=1",
        "-t",
        "3",
        "-pix_fmt",
        "yuv420p",
        str(video_path),
    ]
    proc = await asyncio.create_subprocess_exec(
        *cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    await proc.wait()
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


async def record_ipc_session() -> dict:
    if not WORKER_BIN.exists():
        raise FileNotFoundError(f"C++ worker binary missing at {WORKER_BIN}")

    with tempfile.TemporaryDirectory() as tmp_dir_str:
        tmp_dir = Path(tmp_dir_str)
        socket_path = tmp_dir / "sublift_worker_ipc.sock"
        video_path = await generate_synthetic_video(tmp_dir)

        # Launch C++ worker process
        proc = await asyncio.create_subprocess_exec(
            str(WORKER_BIN),
            "--socket",
            str(socket_path),
            "--engine",
            "mock",
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

        try:
            reader, writer = await connect_unix_with_retry(socket_path)

            # 1. Hello -> Bye
            await send_framed_msg(
                writer,
                {"type": "hello", "client": "dump_ipc_session", "protocol_version": 1},
            )
            bye_msg = await read_framed_msg(reader)

            # 2. StartJob (Path mode)
            await send_framed_msg(
                writer,
                {
                    "type": "start_job",
                    "video_id": "v_session_dump",
                    "fps": 1.0,
                    "engine": "mock",
                    "confidence_threshold": 0.5,
                    "video_path": str(video_path),
                },
            )

            messages = [bye_msg["type"]]

            while True:
                msg = await read_framed_msg(reader)
                messages.append(msg["type"])
                if msg["type"] == "done":
                    break

            writer.close()
            await writer.wait_closed()
        finally:
            try:
                proc.terminate()
                await proc.wait()
            except Exception:
                pass

        return {
            "schema_version": 1,
            "kind": "ipc_session",
            "runtime": bye_msg.get("runtime", "cpp"),
            "engines": bye_msg.get("engines", []),
            "capabilities": bye_msg.get("capabilities", []),
            "message_sequence": messages,
        }


def main() -> None:
    parser = argparse.ArgumentParser(description="Dump or check IPC session golden")
    parser.add_argument("--check", action="store_true", help="Check live session against golden")
    args = parser.parse_args()

    session_data = asyncio.run(record_ipc_session())

    if args.check:
        if not GOLDEN_PATH.exists():
            print(f"Error: Golden file not found at {GOLDEN_PATH}", file=sys.stderr)
            sys.exit(1)

        with open(GOLDEN_PATH, encoding="utf-8") as f:
            golden = json.load(f)

        if session_data["message_sequence"] != golden["message_sequence"]:
            print("Mismatch in IPC message sequence:", file=sys.stderr)
            print(f"Expected: {golden['message_sequence']}", file=sys.stderr)
            print(f"Got:      {session_data['message_sequence']}", file=sys.stderr)
            sys.exit(1)

        print("[OK] IPC session sequence matches golden perfectly.")
    else:
        GOLDEN_PATH.parent.mkdir(parents=True, exist_ok=True)
        with open(GOLDEN_PATH, "w", encoding="utf-8") as f:
            json.dump(session_data, f, indent=2, ensure_ascii=False)
            f.write("\n")
        print(f"Exported golden to {GOLDEN_PATH}")


if __name__ == "__main__":
    main()
