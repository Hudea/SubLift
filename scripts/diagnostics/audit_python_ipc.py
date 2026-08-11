"""Python Oracle IPC 的内存与推送时机诊断。

启动真实的 Python UDS Server，发送 start_job，测量 RSS 物理内存峰值、首条字幕
推送延迟，并测试并发 cancel_job、资源退出和立即重启。它诊断显式 Python Oracle
路径，不替代默认 C++ Worker 的产品门。

用法：
    uv run --extra vision python scripts/diagnostics/audit_python_ipc.py \\
      --video /path/to/video.mp4 --engine vision
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import struct
import subprocess
import sys
import time
from pathlib import Path
from typing import Any


async def read_message(reader: asyncio.StreamReader) -> dict[str, Any] | None:
    try:
        length_bytes = await reader.readexactly(4)
        (length,) = struct.unpack(">I", length_bytes)
        body = await reader.readexactly(length)
        import typing

        return typing.cast(dict[str, Any], json.loads(body.decode("utf-8")))
    except asyncio.IncompleteReadError:
        return None


async def write_message(writer: asyncio.StreamWriter, message: dict[str, Any]) -> None:
    body = json.dumps(message, ensure_ascii=False).encode("utf-8")
    writer.write(struct.pack(">I", len(body)))
    writer.write(body)
    await writer.drain()


async def get_rss_mb(pid: int) -> float:
    """获取进程的 RSS 物理内存占用 (MB)。"""
    try:
        process = await asyncio.create_subprocess_exec(
            "ps",
            "-p",
            str(pid),
            "-o",
            "rss=",
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )
        stdout, _ = await process.communicate()
        rss_kb = int(stdout.decode().strip())
        return rss_kb / 1024.0
    except Exception:
        return 0.0


async def get_child_pids(parent_pid: int) -> list[int]:
    """用 pgrep 获取当前父进程的所有子进程 PID。"""
    try:
        process = await asyncio.create_subprocess_exec(
            "pgrep",
            "-P",
            str(parent_pid),
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )
        stdout, _ = await process.communicate()
        lines = stdout.decode().strip().splitlines()
        return [int(line.strip()) for line in lines if line.strip().isdigit()]
    except Exception:
        return []


async def run_audit(
    *,
    video_path: Path,
    socket_path: Path,
    engine: str,
    fps: float,
    region: list[int],
) -> int:
    print("=== 开始 Python Oracle IPC 诊断 ===")

    if not video_path.is_file():
        print(f"找不到测试视频: {video_path}", file=sys.stderr)
        return 2

    if socket_path.exists():
        print(f"socket 路径已存在，拒绝覆盖: {socket_path}", file=sys.stderr)
        return 2

    # 1. 启动真实的 UDS Server 子进程
    server_process = subprocess.Popen(
        [
            sys.executable,
            "-m",
            "sublift.ipc.server",
            "--socket",
            str(socket_path),
            "--engine",
            engine,
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    print(f"[Server] 已启动子进程 (PID: {server_process.pid})")

    try:
        # 等待 Socket 建立
        for _ in range(20):
            if socket_path.exists():
                break
            await asyncio.sleep(0.1)
        else:
            print("Server 启动超时")
            return 1

        reader, writer = await asyncio.open_unix_connection(str(socket_path))

        start_msg = {
            "type": "start_job",
            "video_id": "audit-test-1",
            "video_path": str(video_path),
            "fps": fps,
            "engine": engine,
            "confidence_threshold": 0.5,
            "enable_ssim_patrol": True,
            "region_box": region,
        }

        print("[Client] 发送 start_job...")
        t0 = time.time()
        await write_message(writer, start_msg)

        first_push_time = None
        max_rss = 0.0
        entries_count = 0
        cancelled = False
        cancel_t0 = 0.0
        ffmpeg_pid = None

        # ----------------------------------------------------
        # 第一阶段：运行 Job 1 并触发 Cancel
        # ----------------------------------------------------
        while True:
            read_task = asyncio.create_task(read_message(reader))
            monitor_task = asyncio.create_task(get_rss_mb(server_process.pid))

            done, _pending = await asyncio.wait(
                [read_task, monitor_task],
                return_when=asyncio.FIRST_COMPLETED,
            )

            if monitor_task in done:
                current_rss = monitor_task.result()
                if current_rss > max_rss:
                    max_rss = current_rss
            else:
                monitor_task.cancel()

            if read_task in done:
                msg = read_task.result()
                if msg is None:
                    break

                msg_type = msg.get("type")

                # 如果 ffmpeg_pid 尚未捕获，尝试从子进程列表定位 ffmpeg
                if ffmpeg_pid is None:
                    children = await get_child_pids(server_process.pid)
                    if children:
                        # ffmpeg 是 python 服务进程派生的子进程
                        ffmpeg_pid = children[0]
                        print(f"[Client] 已捕获 ffmpeg 子进程 (PID: {ffmpeg_pid})")

                if msg_type == "push_entry":
                    entries_count += 1
                    if first_push_time is None:
                        first_push_time = time.time()
                        delay = first_push_time - t0
                        print(f"[Push] 收到首条字幕！延迟: {delay:.2f}s")

                    # 收到 2 条后测试真实 Cancel
                    if entries_count >= 2 and not cancelled:
                        print("[Client] 收到足够数据，发送 cancel_job 测试并发中断...")
                        cancel_t0 = time.time()
                        await write_message(
                            writer, {"type": "cancel_job", "video_id": "audit-test-1"}
                        )
                        cancelled = True
                elif msg_type == "done":
                    cancel_delay = time.time() - cancel_t0 if cancelled else 0
                    print(f"[Done] 收到取消确认，状态: {msg} (取消响应耗时: {cancel_delay:.4f}s)")

                    # 验证 ffmpeg 是否真正退出
                    if ffmpeg_pid is not None:
                        print(f"[Client] 验证 ffmpeg (PID {ffmpeg_pid}) 是否完全退出...")
                        for _ in range(30):
                            current_children = await get_child_pids(server_process.pid)
                            if ffmpeg_pid not in current_children:
                                print(f"[Client] 💚 OK: ffmpeg (PID {ffmpeg_pid}) 已彻底退出")
                                break
                            await asyncio.sleep(0.05)
                        else:
                            print(f"❌ 错误：ffmpeg (PID {ffmpeg_pid}) 在取消后未退出！")
                            sys.exit(1)

                    if cancelled and cancel_delay > 1.0:
                        print(f"❌ 错误：取消响应超时，耗时 {cancel_delay:.4f}s > 1.0s")
                        sys.exit(1)
                    break
            else:
                read_task.cancel()

            await asyncio.sleep(0.05)

        # ----------------------------------------------------
        # 第二阶段：立即在同一 Server 连接下拉起 Job 2
        # ----------------------------------------------------
        print("\n[Client] 立即拉起 Job 2 验证重启能力...")
        start_msg2 = {
            "type": "start_job",
            "video_id": "audit-test-2",
            "video_path": str(video_path),
            "fps": fps,
            "engine": engine,
            "confidence_threshold": 0.5,
            "enable_ssim_patrol": True,
            "region_box": region,
        }
        await write_message(writer, start_msg2)
        job2_t0 = time.time()
        job2_push_received = False

        for _ in range(100):  # 最多等待 5 秒
            read_task = asyncio.create_task(read_message(reader))
            await read_task
            msg2 = read_task.result()
            if msg2 is None:
                break

            if msg2.get("type") == "push_entry":
                print(f"[Push] Job 2 成功收到字幕！延迟: {time.time() - job2_t0:.2f}s")
                job2_push_received = True
                break
            await asyncio.sleep(0.05)

        if not job2_push_received:
            print("❌ 错误：拉起 Job 2 失败，未在 5s 内收到任何 push_entry")
            sys.exit(1)
        else:
            print("💚 OK: Job 2 即时重启且运行正常，双向重启验证通过！")

        # 优雅清理 Job 2
        await write_message(writer, {"type": "cancel_job", "video_id": "audit-test-2"})

        print(f"\n[Memory] Server 子进程物理内存 (RSS) 峰值: {max_rss:.2f} MB")

        # 测试结束，关闭连接
        writer.close()
        await writer.wait_closed()
        return 0

    finally:
        server_process.terminate()
        server_process.wait()
        if socket_path.exists():
            socket_path.unlink()


def _parse_region(raw: str) -> list[int]:
    try:
        region = [int(value) for value in raw.split(",")]
    except ValueError as exc:
        raise argparse.ArgumentTypeError("区域必须是 x,y,width,height") from exc
    if len(region) != 4 or region[2] <= 0 or region[3] <= 0:
        raise argparse.ArgumentTypeError("区域必须是 x,y,width,height，且宽高为正")
    return region


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Python Oracle IPC 内存与推送时机诊断")
    parser.add_argument("--video", type=Path, required=True, help="待诊断的本地视频")
    parser.add_argument(
        "--socket",
        type=Path,
        default=None,
        help="可选 UDS socket 路径；默认使用当前进程专用的 /tmp 路径",
    )
    parser.add_argument("--engine", choices=["vision", "mock", "paddle"], default="vision")
    parser.add_argument("--fps", type=float, default=5.0)
    parser.add_argument("--region", type=_parse_region, default=[0, 860, 1920, 220])
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.fps <= 0:
        print("--fps 必须大于 0", file=sys.stderr)
        return 2
    video_path = args.video.expanduser().resolve()
    socket_path = (
        args.socket.expanduser().resolve()
        if args.socket is not None
        else Path("/tmp") / f"sublift-audit-{os.getpid()}.sock"
    )
    return asyncio.run(
        run_audit(
            video_path=video_path,
            socket_path=socket_path,
            engine=args.engine,
            fps=args.fps,
            region=args.region,
        )
    )


if __name__ == "__main__":
    raise SystemExit(main())
