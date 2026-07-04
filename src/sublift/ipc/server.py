"""SubLift IPC UDS server。

Swift GUI 端通过 `Process` 启动本模块作为子进程，经 Unix Domain Socket 通信。
消息分帧：4 字节大端无符号长度前缀 + UTF-8 JSON body。
feat-014：hello/bye 骨架握手验证通道可用。
feat-015：handler 扩展为 9 类消息分发（7 业务 + 2 控制），业务消息返回 stub 响应。
feat-016：将 handler 接入 Pipeline（stub 换成真实 Pipeline 调用）。
"""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import struct
import sys
from collections.abc import Awaitable, Callable
from typing import Any, Protocol

from sublift.ipc.protocol import (
    MSG_CANCEL_JOB,
    MSG_FRAME,
    MSG_HELLO,
    MSG_START_JOB,
    ProtocolError,
    build_bye,
    build_done,
    build_error,
    build_progress,
    validate,
)

logger = logging.getLogger(__name__)

# 消息分帧：4 字节大端无符号整数表示 JSON body 长度
LENGTH_PREFIX_SIZE = 4
MAX_MESSAGE_BYTES = 64 * 1024 * 1024  # 64 MiB 上限，防止异常大帧 OOM


class StreamWriter(Protocol):
    """StreamWriter 协议，兼容 asyncio.StreamWriter 与测试 mock。"""

    def write(self, data: bytes) -> None: ...
    async def drain(self) -> None: ...
    def close(self) -> None: ...
    async def wait_closed(self) -> None: ...
    def get_extra_info(self, name: str, default: Any = None) -> Any: ...


async def read_message(reader: asyncio.StreamReader) -> dict[str, Any] | None:
    """从 stream reader 读取一条分帧消息。

    Args:
        reader: asyncio StreamReader（已连接）。

    Returns:
        解析后的 JSON 字典；连接正常关闭时返回 None。

    Raises:
        ValueError: 消息长度超限或 JSON 解析失败。
    """
    try:
        length_bytes = await reader.readexactly(LENGTH_PREFIX_SIZE)
    except asyncio.IncompleteReadError:
        return None
    (length,) = struct.unpack(">I", length_bytes)
    if length == 0 or length > MAX_MESSAGE_BYTES:
        raise ValueError(f"消息长度超限或为零: {length}")

    body = await reader.readexactly(length)
    result: dict[str, Any] = json.loads(body.decode("utf-8"))
    return result


async def write_message(writer: StreamWriter, message: dict[str, Any]) -> None:
    """向 stream writer 写入一条分帧消息。

    Args:
        writer: asyncio StreamWriter（已连接）。
        message: 待发送的消息字典。
    """
    body = json.dumps(message, ensure_ascii=False).encode("utf-8")
    writer.write(struct.pack(">I", len(body)))
    writer.write(body)
    await writer.drain()


async def handle_connection(
    reader: asyncio.StreamReader,
    writer: StreamWriter,
    *,
    handler: Callable[[dict[str, Any]], Awaitable[dict[str, Any] | None]] | None = None,
) -> None:
    """处理单个连接：读取消息，调用 handler，写回响应，直到 handler 返回 None。

    Args:
        reader: StreamReader。
        writer: StreamWriter。
        handler: 消息处理回调，返回响应字典或 None（关闭连接）。None 时用默认分发 handler。
    """
    if handler is None:
        handler = _default_handler
    peer = writer.get_extra_info("peername")
    logger.debug("UDS 连接已建立: %s", peer)

    try:
        while True:
            message = await read_message(reader)
            if message is None:
                break

            response = await handler(message)
            if response is not None:
                await write_message(writer, response)

            if response is None or message.get("type") == "bye":
                break
    except asyncio.IncompleteReadError:
        logger.debug("连接被对端关闭")
    except Exception:
        logger.exception("处理连接时发生异常")
    finally:
        writer.close()
        await writer.wait_closed()
        logger.debug("UDS 连接已关闭: %s", peer)


async def _default_handler(message: dict[str, Any]) -> dict[str, Any] | None:
    """默认消息分发 handler。

    feat-015：9 类消息分发（7 业务 stub + 2 控制握手）。
    业务消息先经 protocol.validate() 校验 schema，再返回 stub 响应。
    feat-016 将把 stub 换成真实 Pipeline 调用。

    Args:
        message: 接收到的消息。

    Returns:
        响应消息字典，或 None 表示关闭连接。
    """
    msg_type = message.get("type")

    if msg_type == MSG_HELLO:
        return build_bye()
    if msg_type == "bye":
        return None

    try:
        validate(message)
    except ProtocolError as e:
        return build_error(str(e))

    if msg_type == MSG_START_JOB:
        video_id = message["video_id"]
        return build_progress(video_id, "ready", 0.0, 0)
    if msg_type == MSG_FRAME:
        video_id = message["video_id"]
        return build_progress(video_id, "frame_received", 0.0, 0)
    if msg_type == MSG_CANCEL_JOB:
        video_id = message["video_id"]
        return build_done(video_id, ok=True)
    if msg_type == "bye":
        return None

    return build_error(f"unknown type: {msg_type!r}")


async def serve_once(socket_path: str) -> None:
    """监听 UDS，处理单个连接后退出。

    feat-014 骨架行为：接受一个连接，hello/bye 往返一次，关闭并退出。
    feat-016 将改为持续监听。

    Args:
        socket_path: UDS 路径。
    """
    server = await asyncio.start_unix_server(
        lambda r, w: handle_connection(r, w), path=socket_path
    )
    logger.info("UDS server 监听中: %s", socket_path)
    try:
        async with server:
            await server.serve_forever()
    except asyncio.CancelledError:
        pass
    finally:
        server.close()
        await server.wait_closed()


def build_parser() -> argparse.ArgumentParser:
    """构造命令行参数解析器。"""
    parser = argparse.ArgumentParser(
        prog="sublift.ipc.server",
        description="SubLift IPC UDS server。",
    )
    parser.add_argument(
        "--socket",
        required=True,
        help="Unix Domain Socket 路径",
    )
    parser.add_argument(
        "--log-level",
        default="INFO",
        choices=["DEBUG", "INFO", "WARNING", "ERROR"],
        help="日志级别（默认 INFO）",
    )
    return parser


def main(argv: list[str] | None = None) -> None:
    """CLI 入口。

    Args:
        argv: 命令行参数，None 时用 sys.argv。
    """
    parser = build_parser()
    args = parser.parse_args(argv)

    logging.basicConfig(
        level=args.log_level,
        format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
        stream=sys.stderr,
    )

    try:
        asyncio.run(serve_once(args.socket))
    except KeyboardInterrupt:
        logger.info("收到中断信号，退出")


if __name__ == "__main__":
    main()
