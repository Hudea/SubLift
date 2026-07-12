"""增量架构内存与推送时机端到端审计。

启动真实的 UDS Server，发送 start_job，测量 RSS 物理内存峰值、首条字幕推送延迟，
并测试并发发送 cancel_job 的安全性。

用法：
    uv run --extra vision python scripts/audit_memory_push.py
"""

from __future__ import annotations

import asyncio
import json
import os
import struct
import subprocess
import time
from typing import Any

# 测试视频配置
VIDEO_PATH = "debug/Zootopia_clip_1080p.mp4"
SOCKET_PATH = "/tmp/sublift_audit.sock"


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
            "ps", "-p", str(pid), "-o", "rss=",
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )
        stdout, _ = await process.communicate()
        rss_kb = int(stdout.decode().strip())
        return rss_kb / 1024.0
    except Exception:
        return 0.0


async def run_audit() -> None:
    print("=== 开始真实 IPC 增量架构审计 ===")

    if not os.path.exists(VIDEO_PATH):
        print(f"找不到测试视频: {VIDEO_PATH}")
        return

    if os.path.exists(SOCKET_PATH):
        os.remove(SOCKET_PATH)

    # 1. 启动真实的 UDS Server 子进程
    server_process = subprocess.Popen(
        ["python", "-m", "sublift.ipc.server", "--socket", SOCKET_PATH],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    print(f"[Server] 已启动子进程 (PID: {server_process.pid})")

    try:
        # 等待 Socket 建立
        for _ in range(20):
            if os.path.exists(SOCKET_PATH):
                break
            await asyncio.sleep(0.1)
        else:
            print("Server 启动超时")
            return

        reader, writer = await asyncio.open_unix_connection(SOCKET_PATH)

        start_msg = {
            "type": "start_job",
            "video_id": "audit-test-1",
            "video_path": VIDEO_PATH,
            "fps": 5.0,
            "engine": "vision",
            "confidence_threshold": 0.5,
            "enable_ssim_patrol": True,
            "region_box": [0, 860, 1920, 220],
        }

        print("[Client] 发送 start_job...")
        t0 = time.time()
        await write_message(writer, start_msg)

        first_push_time = None
        max_rss = 0.0
        entries_count = 0
        cancelled = False

        while True:
            # 并发读取消息和监控内存
            read_task = asyncio.create_task(read_message(reader))
            monitor_task = asyncio.create_task(get_rss_mb(server_process.pid))

            done, _pending = await asyncio.wait(
                [read_task, monitor_task],
                return_when=asyncio.FIRST_COMPLETED,
            )

            # 更新内存峰值
            if monitor_task in done:
                current_rss = monitor_task.result()
                if current_rss > max_rss:
                    max_rss = current_rss
            else:
                monitor_task.cancel()

            # 处理收到的消息
            if read_task in done:
                msg = read_task.result()
                if msg is None:
                    break

                msg_type = msg.get("type")
                if msg_type == "push_entry":
                    entries_count += 1
                    if first_push_time is None:
                        first_push_time = time.time()
                        delay = first_push_time - t0
                        print(f"[Push] 收到首条字幕！延迟: {delay:.2f}s")

                    # 收到 2 条后测试真实 Cancel
                    if entries_count >= 2 and not cancelled:
                        print("[Client] 收到足够数据，发送 cancel_job 测试并发中断...")
                        await write_message(
                            writer, {"type": "cancel_job", "video_id": "audit-test-1"}
                        )
                        cancelled = True
                elif msg_type == "done":
                    print(f"[Done] 提取结束，状态: {msg}")
                    break
            else:
                read_task.cancel()

            await asyncio.sleep(0.05)

        print(f"\n[Memory] Server 子进程物理内存 (RSS) 峰值: {max_rss:.2f} MB")

        # 测试结束，关闭连接
        writer.close()
        await writer.wait_closed()

    finally:
        server_process.terminate()
        server_process.wait()
        if os.path.exists(SOCKET_PATH):
            os.remove(SOCKET_PATH)


if __name__ == "__main__":
    asyncio.run(run_audit())
