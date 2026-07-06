"""SubLift IPC UDS server。

Swift GUI 端通过 `Process` 启动本模块作为子进程，经 Unix Domain Socket 通信。
消息分帧：4 字节大端无符号长度前缀 + UTF-8 JSON body。
feat-014：hello/bye 骨架握手验证通道可用。
feat-015：handler 扩展为 9 类消息分发（7 业务 + 2 控制），业务消息返回 stub 响应。
feat-016：handler 接入真实 Pipeline（bridge.BridgeHandler）。
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
        handler: 消息处理回调，返回响应字典或 None（关闭连接）。None 时用默认 BridgeHandler。
    """
    if handler is None:
        from sublift.ipc.bridge import BridgeHandler

        bridge = BridgeHandler()
        handler = bridge.handle
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


async def serve_once(
    socket_path: str,
    *,
    handler_factory: Callable[[], Callable[[dict[str, Any]], Awaitable[dict[str, Any] | None]]]
    | None = None,
) -> None:
    """监听 UDS，持续接受连接并处理。

    Args:
        socket_path: UDS 路径。
        handler_factory: 可选的 handler 工厂函数，每次连接创建一个新 handler。
            None 时用默认 BridgeHandler。
    """
    if handler_factory is None:
        from sublift.ipc.bridge import BridgeHandler

        default_bridge = BridgeHandler()
        handler_factory = lambda: default_bridge.handle  # noqa: E731

    server = await asyncio.start_unix_server(
        lambda r, w: handle_connection(r, w, handler=handler_factory()),
        path=socket_path,
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
    parser.add_argument(
        "--engine",
        default="vision",
        choices=["vision", "mock"],
        help="OCR 引擎（默认 vision，测试可用 mock）",
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

    # 根据 --engine 参数选择 OCR 引擎工厂
    if args.engine == "mock":
        from sublift.ipc.bridge import BridgeHandler
        from sublift.ocr.mock import MockOcrEngine

        handler_factory = lambda: BridgeHandler(  # noqa: E731
            ocr_engine_factory=MockOcrEngine
        ).handle
    else:
        handler_factory = None  # 默认用 VisionOcrEngine

    try:
        asyncio.run(serve_once(args.socket, handler_factory=handler_factory))
    except KeyboardInterrupt:
        logger.info("收到中断信号，退出")


if __name__ == "__main__":
    main()
