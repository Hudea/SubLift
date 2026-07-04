"""IPC UDS server 单测。

feat-014 骨架验证：
- hello/bye 往返成功
- 消息分帧（4 字节长度前缀 + JSON body）正确
- 连接关闭后 server 退出
不依赖 Swift，纯 Python 自连。
"""

from __future__ import annotations

import asyncio
import json
import struct
import uuid
from pathlib import Path

import pytest

from sublift.ipc.server import (
    LENGTH_PREFIX_SIZE,
    handle_connection,
    read_message,
    serve_once,
    write_message,
)


def _pack(message: dict[str, object]) -> bytes:
    """手工打包一条分帧消息，用于单测。"""
    body = json.dumps(message, ensure_ascii=False).encode("utf-8")
    return struct.pack(">I", len(body)) + body


def _parse_response(chunks: bytes) -> dict[str, object]:
    """从 _MockStreamWriter 捕获的字节中解析一条响应消息。"""
    (length,) = struct.unpack(">I", chunks[:LENGTH_PREFIX_SIZE])
    body = chunks[LENGTH_PREFIX_SIZE : LENGTH_PREFIX_SIZE + length]
    result: dict[str, object] = json.loads(body.decode("utf-8"))
    return result


class _MockStreamWriter:
    """StreamWriter 替身：捕获写入字节，便于单测。

    不继承 asyncio.StreamWriter，避免 _transport 等内部属性依赖。
    只实现 handle_connection 用到的方法：write/drain/close/wait_closed/get_extra_info。
    """

    def __init__(self) -> None:
        self.chunks = b""
        self._closed = False

    def write(self, data: bytes) -> None:
        self.chunks += data

    async def drain(self) -> None:
        pass

    def close(self) -> None:
        self._closed = True

    async def wait_closed(self) -> None:
        pass

    def get_extra_info(self, name: str, default: object = None) -> object:
        return default


class TestFraming:
    """消息分帧单元测试。"""

    def test_read_message_parses_hello(self) -> None:
        """read_message 正确解析分帧 hello。"""

        async def scenario() -> dict[str, object] | None:
            reader = asyncio.StreamReader()
            reader.feed_data(_pack({"type": "hello", "client": "test"}))
            return await read_message(reader)

        message = asyncio.run(scenario())
        assert message == {"type": "hello", "client": "test"}

    def test_read_message_returns_none_on_eof(self) -> None:
        """连接关闭时 read_message 返回 None。"""

        async def scenario() -> dict[str, object] | None:
            reader = asyncio.StreamReader()
            reader.feed_eof()
            return await read_message(reader)

        message = asyncio.run(scenario())
        assert message is None

    def test_read_message_rejects_zero_length(self) -> None:
        """零长度消息应报错。"""

        async def scenario() -> dict[str, object] | None:
            reader = asyncio.StreamReader()
            reader.feed_data(struct.pack(">I", 0))
            return await read_message(reader)

        with pytest.raises(ValueError, match="超限"):
            asyncio.run(scenario())

    def test_write_message_format(self) -> None:
        """write_message 输出格式 = 4 字节长度前缀 + UTF-8 JSON body。"""

        async def scenario() -> bytes:
            writer = _MockStreamWriter()
            await write_message(writer, {"type": "bye"})
            return writer.chunks

        chunks = asyncio.run(scenario())
        (length,) = struct.unpack(">I", chunks[:LENGTH_PREFIX_SIZE])
        body = chunks[LENGTH_PREFIX_SIZE : LENGTH_PREFIX_SIZE + length]
        assert json.loads(body.decode("utf-8")) == {"type": "bye"}
        assert len(chunks) == LENGTH_PREFIX_SIZE + length

    def test_pack_matches_write_format(self) -> None:
        """_pack 手工打包与 write_message 输出格式一致。"""

        async def scenario() -> bytes:
            writer = _MockStreamWriter()
            await write_message(writer, {"type": "bye"})
            return writer.chunks

        chunks = asyncio.run(scenario())
        assert chunks == _pack({"type": "bye"})


class TestHandleConnection:
    """handle_connection 单元测试（不启动真实 server）。"""

    def test_hello_returns_bye(self) -> None:
        """发 hello，handler 返回 bye。"""

        async def scenario() -> bytes:
            reader = asyncio.StreamReader()
            writer = _MockStreamWriter()
            reader.feed_data(_pack({"type": "hello", "client": "test"}))
            reader.feed_eof()
            await handle_connection(reader, writer)
            return writer.chunks

        chunks = asyncio.run(scenario())
        response = _parse_response(chunks)
        assert response == {"type": "bye"}

    def test_bye_closes_connection(self) -> None:
        """发 bye，handler 返回 None，连接关闭。"""

        async def scenario() -> bytes:
            reader = asyncio.StreamReader()
            writer = _MockStreamWriter()
            reader.feed_data(_pack({"type": "bye"}))
            reader.feed_eof()
            await handle_connection(reader, writer)
            return writer.chunks

        chunks = asyncio.run(scenario())
        assert chunks == b""

    def test_unknown_type_returns_error(self) -> None:
        """未知消息类型返回 error 响应。"""

        async def scenario() -> bytes:
            reader = asyncio.StreamReader()
            writer = _MockStreamWriter()
            reader.feed_data(_pack({"type": "unknown_thing"}))
            reader.feed_eof()
            await handle_connection(reader, writer)
            return writer.chunks

        chunks = asyncio.run(scenario())
        response = _parse_response(chunks)
        assert response["type"] == "error"
        message = response["message"]
        assert isinstance(message, str)
        assert "unknown_thing" in message


class TestServeOnce:
    """serve_once 端到端单测：真实启动 UDS server。"""

    def test_hello_bye_roundtrip(self) -> None:
        """启动 server，客户端发 hello 收 bye，server 处理完退出。"""
        # macOS AF_UNIX 路径上限 104 字节，用 /tmp 避免 pytest tmp_path 过长
        socket_path = f"/tmp/sublift-test-{uuid.uuid4().hex[:8]}.sock"

        async def scenario() -> dict[str, object]:
            server_task = asyncio.create_task(serve_once(socket_path))

            for _ in range(50):
                if Path(socket_path).exists():
                    break
                await asyncio.sleep(0.01)
            else:
                server_task.cancel()
                pytest.fail("server 未在 0.5s 内启动")

            try:
                reader, writer = await asyncio.open_unix_connection(socket_path)
                try:
                    await write_message(writer, {"type": "hello", "client": "sublift-mac"})
                    response = await read_message(reader)
                    assert response is not None
                    return response
                finally:
                    writer.close()
                    await writer.wait_closed()
            finally:
                await asyncio.wait_for(server_task, timeout=2.0)

        response = asyncio.run(scenario())
        assert response == {"type": "bye"}

        # 清理 socket 文件
        Path(socket_path).unlink(missing_ok=True)

    def test_server_exits_after_connection_closes(self) -> None:
        """server 处理完连接后自动退出。"""
        socket_path = f"/tmp/sublift-test-{uuid.uuid4().hex[:8]}.sock"

        async def scenario() -> None:
            server_task = asyncio.create_task(serve_once(socket_path))

            for _ in range(50):
                if Path(socket_path).exists():
                    break
                await asyncio.sleep(0.01)

            reader, writer = await asyncio.open_unix_connection(socket_path)
            await write_message(writer, {"type": "hello", "client": "test"})
            await read_message(reader)
            writer.close()
            await writer.wait_closed()

            # server 处理完一个连接后应退出
            await asyncio.wait_for(server_task, timeout=2.0)

        asyncio.run(scenario())
        Path(socket_path).unlink(missing_ok=True)
