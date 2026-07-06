"""IPC UDS server 单测。

feat-014：hello/bye 往返 + 分帧格式 + 连接关闭后 server 退出。
feat-015：消息 schema 校验。
feat-016：handler 接入 BridgeHandler（用 MockOcrEngine 避免依赖 Vision）。
不依赖 Swift，纯 Python 自连。
"""

from __future__ import annotations

import asyncio
import json
import struct
import uuid
from pathlib import Path
from typing import Any

import pytest

from sublift.ipc.bridge import BridgeHandler
from sublift.ipc.protocol import (
    build_cancel_job,
    build_finalize,
    build_frame,
    build_hello,
    build_start_job,
)
from sublift.ipc.server import (
    LENGTH_PREFIX_SIZE,
    handle_connection,
    read_message,
    serve_once,
    write_message,
)
from sublift.ocr.mock import MockOcrEngine


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
    """handle_connection 单元测试（不启动真实 server）。

    用 MockOcrEngine 避免依赖 Vision。
    """

    def _make_handler(self) -> BridgeHandler:
        return BridgeHandler(ocr_engine_factory=MockOcrEngine)

    def test_hello_returns_bye(self) -> None:
        """发 hello，handler 返回 bye。"""

        async def scenario() -> bytes:
            reader = asyncio.StreamReader()
            writer = _MockStreamWriter()
            bridge = self._make_handler()
            reader.feed_data(_pack(build_hello("test")))
            reader.feed_eof()
            await handle_connection(reader, writer, handler=bridge.handle)
            return writer.chunks

        chunks = asyncio.run(scenario())
        response = _parse_response(chunks)
        assert response == {"type": "bye"}

    def test_bye_closes_connection(self) -> None:
        """发 bye，handler 返回 None，连接关闭。"""

        async def scenario() -> bytes:
            reader = asyncio.StreamReader()
            writer = _MockStreamWriter()
            bridge = self._make_handler()
            reader.feed_data(_pack({"type": "bye"}))
            reader.feed_eof()
            await handle_connection(reader, writer, handler=bridge.handle)
            return writer.chunks

        chunks = asyncio.run(scenario())
        assert chunks == b""

    def test_schema_error_returns_error(self) -> None:
        """schema 校验失败（缺 video_id）返回 error 响应。"""

        async def scenario() -> bytes:
            reader = asyncio.StreamReader()
            writer = _MockStreamWriter()
            bridge = self._make_handler()
            reader.feed_data(_pack({"type": "start_job", "fps": 5.0}))
            reader.feed_eof()
            await handle_connection(reader, writer, handler=bridge.handle)
            return writer.chunks

        chunks = asyncio.run(scenario())
        response = _parse_response(chunks)
        assert response["type"] == "error"
        message = response["message"]
        assert isinstance(message, str)
        assert "video_id" in message


class TestBusinessMessageStubs:
    """feat-015/016：业务消息的 handler 分发（用 MockOcrEngine）。"""

    def _make_handler(self) -> BridgeHandler:
        return BridgeHandler(ocr_engine_factory=MockOcrEngine)

    def test_start_job_returns_progress_ready(self) -> None:
        """start_job → progress(stage=ready)。"""

        async def scenario() -> bytes:
            reader = asyncio.StreamReader()
            writer = _MockStreamWriter()
            bridge = self._make_handler()
            reader.feed_data(
                _pack(build_start_job("V1", 5.0, "vision", 0.5))
            )
            reader.feed_eof()
            await handle_connection(reader, writer, handler=bridge.handle)
            return writer.chunks

        chunks = asyncio.run(scenario())
        response = _parse_response(chunks)
        assert response["type"] == "progress"
        assert response["video_id"] == "V1"
        assert response["stage"] == "ready"

    def test_frame_returns_progress_processing(self) -> None:
        """frame → progress(stage=processing)。"""
        # 需要先 start_job，否则 frame 在 pipeline 未构造时报错

        async def scenario() -> bytes:
            reader = asyncio.StreamReader()
            writer = _MockStreamWriter()
            bridge = self._make_handler()
            # 先 start_job
            reader.feed_data(
                _pack(build_start_job("V1", 5.0, "vision", 0.5))
            )
            # 再 frame（用一个最小有效 JPEG）
            from PIL import Image

            img = Image.new("RGB", (320, 240), color="black")
            import io

            buf = io.BytesIO()
            img.save(buf, format="JPEG")
            reader.feed_data(_pack(build_frame("V1", 1000, buf.getvalue())))
            reader.feed_eof()
            await handle_connection(reader, writer, handler=bridge.handle)
            return writer.chunks

        chunks = asyncio.run(scenario())
        # 应该有两条响应：start_job→progress(ready) + frame→progress(processing)
        # 解析第二条
        (len1,) = struct.unpack(">I", chunks[:LENGTH_PREFIX_SIZE])
        offset = LENGTH_PREFIX_SIZE + len1
        (len2,) = struct.unpack(">I", chunks[offset : offset + LENGTH_PREFIX_SIZE])
        body2 = chunks[offset + LENGTH_PREFIX_SIZE : offset + LENGTH_PREFIX_SIZE + len2]
        response = json.loads(body2.decode("utf-8"))
        assert response["type"] == "progress"
        assert response["stage"] == "processing"

    def test_cancel_job_returns_done(self) -> None:
        """cancel_job → done(ok=False, error=cancelled)。"""

        async def scenario() -> bytes:
            reader = asyncio.StreamReader()
            writer = _MockStreamWriter()
            bridge = self._make_handler()
            reader.feed_data(_pack(build_cancel_job("V1")))
            reader.feed_eof()
            await handle_connection(reader, writer, handler=bridge.handle)
            return writer.chunks

        chunks = asyncio.run(scenario())
        response = _parse_response(chunks)
        assert response["type"] == "done"
        assert response["video_id"] == "V1"
        assert response["ok"] is False
        assert response["error"] == "cancelled"


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

    def test_full_message_sequence_over_real_socket(self) -> None:
        """feat-016 真实跨进程测试：UDS server（MockOcrEngine）完整消息序列。

        验证分帧 + BridgeHandler 分发 + Pipeline.run_frames 在真实 I/O 下正常工作。
        """
        import io

        from PIL import Image

        socket_path = f"/tmp/sublift-test-{uuid.uuid4().hex[:8]}.sock"

        # 构造一个有效 JPEG 帧
        img = Image.new("RGB", (320, 240), color="black")
        buf = io.BytesIO()
        img.save(buf, format="JPEG")
        jpeg_bytes = buf.getvalue()

        def make_handler() -> Any:
            return BridgeHandler(ocr_engine_factory=MockOcrEngine).handle

        async def scenario() -> list[dict[str, object] | None]:
            server_task = asyncio.create_task(
                serve_once(socket_path, handler_factory=make_handler)
            )

            for _ in range(50):
                if Path(socket_path).exists():
                    break
                await asyncio.sleep(0.01)
            else:
                server_task.cancel()
                pytest.fail("server 未在 0.5s 内启动")

            responses: list[dict[str, object] | None] = []
            try:
                reader, writer = await asyncio.open_unix_connection(socket_path)

                # 1. hello → bye
                await write_message(writer, build_hello("sublift-mac"))
                responses.append(await read_message(reader))

                # 2. start_job → progress(stage=ready)
                await write_message(
                    writer, build_start_job("V1", 5.0, "vision", 0.5, region_box=[0, 0, 1920, 1080])
                )
                responses.append(await read_message(reader))

                # 3. frame → progress(stage=processing)
                await write_message(writer, build_frame("V1", 1000, jpeg_bytes))
                responses.append(await read_message(reader))

                # 4. finalize → entries
                await write_message(writer, build_finalize("V1"))
                responses.append(await read_message(reader))

                # 5. bye → None（连接关闭）
                await write_message(writer, {"type": "bye"})
                responses.append(await read_message(reader))

                writer.close()
                await writer.wait_closed()
            finally:
                await asyncio.wait_for(server_task, timeout=5.0)

            return responses

        responses = asyncio.run(scenario())
        Path(socket_path).unlink(missing_ok=True)

        # 验证 5 条响应
        assert responses[0] == {"type": "bye"}
        assert responses[1] == {
            "type": "progress",
            "video_id": "V1",
            "stage": "ready",
            "pct": 0.0,
            "eta_ms": 0,
        }
        assert responses[2] is not None
        assert responses[2]["type"] == "progress"
        assert responses[2]["stage"] == "processing"
        # finalize → entries（MockOcrEngine 返回固定文本，可能 0 或 1 条）
        assert responses[3] is not None
        assert responses[3]["type"] == "entries"
        # bye 后 handler 返回 None，read_message 收到 EOF 返回 None
        assert responses[4] is None
