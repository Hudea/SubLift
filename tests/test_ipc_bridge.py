"""Bridge handler 单测（feat-016）。

用 MockOcrEngine 跑真实 Pipeline.run_frames，验证完整流程：
- start_job → 构造 Pipeline
- frame → JPEG 解码 + 缓冲
- finalize → 调 run_frames → 回传 entries
- cancel_job → 清空缓冲
- 安全限制：帧数上限 / JPEG 大小上限 / 无效 JPEG
"""

from __future__ import annotations

import asyncio
import io
from typing import Any
from unittest.mock import patch

import pytest
from PIL import Image

from sublift.ipc.bridge import MAX_JPEG_BYTES, BridgeHandler
from sublift.ipc.protocol import (
    build_cancel_job,
    build_finalize,
    build_frame,
    build_start_job,
)
from sublift.ocr.mock import MockOcrEngine


def _make_jpeg(width: int = 320, height: int = 240, color: str = "black") -> bytes:
    """生成一个有效的最小 JPEG。"""
    img = Image.new("RGB", (width, height), color=color)
    buf = io.BytesIO()
    img.save(buf, format="JPEG")
    return buf.getvalue()


def _make_handler() -> BridgeHandler:
    return BridgeHandler(ocr_engine_factory=MockOcrEngine)


def _run(bridge: BridgeHandler, msg: dict[str, Any]) -> dict[str, Any] | None:
    """便捷包装：同步调用 bridge.handle。"""
    return asyncio.run(bridge.handle(msg))


class TestStartJob:
    def test_start_job_returns_progress_ready(self) -> None:
        bridge = _make_handler()
        msg = build_start_job("V1", 5.0, "vision", 0.5)
        response = _run(bridge, msg)
        assert response is not None
        assert response["type"] == "progress"
        assert response["stage"] == "ready"
        assert response["video_id"] == "V1"

    def test_start_job_with_duration_estimates_frames(self) -> None:
        bridge = _make_handler()
        msg = build_start_job("V1", 5.0, "vision", 0.5, duration_ms=10000)
        response = _run(bridge, msg)
        assert response is not None
        assert response["stage"] == "ready"

    def test_start_job_invalid_engine_returns_error(self) -> None:
        bridge = _make_handler()
        msg = build_start_job("V1", 5.0, "paddle", 0.5)
        response = _run(bridge, msg)
        assert response is not None
        assert response["type"] == "error"

    def test_start_job_vision_unavailable_returns_done_error(self) -> None:
        """VisionOcrEngine 构造失败时应返回 done(ok=False)。"""
        bridge = BridgeHandler()  # 默认 VisionOcrEngine
        with patch("sublift.ocr.vision.VisionOcrEngine.__init__") as mock_init:
            mock_init.side_effect = RuntimeError("Vision 不可用")
            msg = build_start_job("V1", 5.0, "vision", 0.5)
            response = _run(bridge, msg)
        assert response is not None
        assert response["type"] == "done"
        assert response["ok"] is False


class TestFrame:
    def test_frame_before_start_job_returns_error(self) -> None:
        bridge = _make_handler()
        jpeg = _make_jpeg()
        msg = build_frame("V1", 1000, jpeg)
        response = _run(bridge, msg)
        assert response is not None
        assert response["type"] == "error"
        assert "before start_job" in response["message"]

    def test_frame_returns_progress(self) -> None:
        bridge = _make_handler()
        _run(bridge, build_start_job("V1", 5.0, "vision", 0.5))
        jpeg = _make_jpeg()
        msg = build_frame("V1", 1000, jpeg)
        response = _run(bridge, msg)
        assert response is not None
        assert response["type"] == "progress"
        assert response["stage"] == "frame_received"

    def test_frame_with_duration_updates_pct(self) -> None:
        bridge = _make_handler()
        _run(bridge, build_start_job("V1", 5.0, "vision", 0.5, duration_ms=1000))
        # duration_ms=1000, fps=5 → est_total=5 frames
        jpeg = _make_jpeg()
        _run(bridge, build_frame("V1", 0, jpeg))
        response = _run(bridge, build_frame("V1", 200, jpeg))
        assert response is not None
        assert response["type"] == "progress"
        # 2 frames out of 5 = 0.4
        assert response["pct"] == pytest.approx(0.4)

    def test_frame_invalid_jpeg_returns_error(self) -> None:
        bridge = _make_handler()
        _run(bridge, build_start_job("V1", 5.0, "vision", 0.5))
        msg = build_frame("V1", 1000, b"not a jpeg")
        response = _run(bridge, msg)
        assert response is not None
        assert response["type"] == "error"
        assert "JPEG" in response["message"] or "decode" in response["message"].lower()

    def test_frame_invalid_base64_returns_error(self) -> None:
        bridge = _make_handler()
        _run(bridge, build_start_job("V1", 5.0, "vision", 0.5))
        # 直接构造非法 base64 的消息
        msg: dict[str, Any] = {
            "type": "frame",
            "video_id": "V1",
            "ts_ms": 1000,
            "jpeg_bytes": "!!!invalid base64!!!",
            "region_box": None,
        }
        response = _run(bridge, msg)
        assert response is not None
        assert response["type"] == "error"


class TestFinalize:
    def test_finalize_returns_entries(self) -> None:
        """完整的 start_job → frame → finalize 流程。"""
        bridge = _make_handler()
        _run(bridge, build_start_job("V1", 5.0, "vision", 0.5))

        jpeg = _make_jpeg()
        _run(bridge, build_frame("V1", 0, jpeg))
        _run(bridge, build_frame("V1", 200, jpeg))
        _run(bridge, build_frame("V1", 400, jpeg))

        response = _run(bridge, build_finalize("V1"))
        assert response is not None
        assert response["type"] == "entries"
        assert response["video_id"] == "V1"
        assert isinstance(response["entries"], list)

    def test_finalize_before_start_job_returns_error(self) -> None:
        bridge = _make_handler()
        response = _run(bridge, build_finalize("V1"))
        assert response is not None
        assert response["type"] == "error"
        assert "before start_job" in response["message"]

    def test_finalize_empty_frames_returns_empty_entries(self) -> None:
        """start_job 后不推任何帧直接 finalize。"""
        bridge = _make_handler()
        _run(bridge, build_start_job("V1", 5.0, "vision", 0.5))
        response = _run(bridge, build_finalize("V1"))
        assert response is not None
        assert response["type"] == "entries"
        assert response["entries"] == []


class TestCancelJob:
    def test_cancel_returns_done_cancelled(self) -> None:
        bridge = _make_handler()
        _run(bridge, build_start_job("V1", 5.0, "vision", 0.5))
        response = _run(bridge, build_cancel_job("V1"))
        assert response is not None
        assert response["type"] == "done"
        assert response["ok"] is False
        assert response["error"] == "cancelled"

    def test_cancel_clears_buffer(self) -> None:
        """cancel 后再 finalize 应报错（pipeline 已清空）。"""
        bridge = _make_handler()
        _run(bridge, build_start_job("V1", 5.0, "vision", 0.5))
        jpeg = _make_jpeg()
        _run(bridge, build_frame("V1", 0, jpeg))
        _run(bridge, build_cancel_job("V1"))
        response = _run(bridge, build_finalize("V1"))
        assert response is not None
        assert response["type"] == "error"


class TestSafetyLimits:
    def test_frame_count_limit(self) -> None:
        """超过 MAX_FRAMES 应返回 done(ok=False)。"""
        bridge = _make_handler()
        _run(bridge, build_start_job("V1", 5.0, "vision", 0.5))
        jpeg = _make_jpeg(width=10, height=10)

        with patch("sublift.ipc.bridge.MAX_FRAMES", 3):
            for i in range(3):
                r = _run(bridge, build_frame("V1", i * 200, jpeg))
                assert r is not None
                assert r["type"] == "progress"

            # 第 4 帧应触发上限
            r = _run(bridge, build_frame("V1", 600, jpeg))
        assert r is not None
        assert r["type"] == "done"
        assert r["ok"] is False
        assert "帧数超限" in r["error"]

    def test_jpeg_size_limit(self) -> None:
        """超过 MAX_JPEG_BYTES 应返回 error。"""
        bridge = _make_handler()
        _run(bridge, build_start_job("V1", 5.0, "vision", 0.5))

        # 构造一个超过限制的 JPEG（通过 mock）
        big_jpeg = b"\xff\xd8\xff\xe0" + b"\x00" * (MAX_JPEG_BYTES + 1)
        msg = build_frame("V1", 1000, big_jpeg)
        response = _run(bridge, msg)
        assert response is not None
        assert response["type"] == "error"
        assert "JPEG 过大" in response["message"]

    def test_total_pixels_limit(self) -> None:
        """总像素超限应返回 done(ok=False)。"""
        bridge = _make_handler()
        _run(bridge, build_start_job("V1", 5.0, "vision", 0.5))
        jpeg = _make_jpeg(width=320, height=240)  # 76800 pixels per frame

        # 设总像素上限为 2 帧，第 3 帧应触发
        with patch("sublift.ipc.bridge.MAX_TOTAL_PIXELS", 76800 * 2):
            _run(bridge, build_frame("V1", 0, jpeg))
            _run(bridge, build_frame("V1", 200, jpeg))
            r = _run(bridge, build_frame("V1", 400, jpeg))

        assert r is not None
        assert r["type"] == "done"
        assert r["ok"] is False
        assert "总像素超限" in r["error"]
