"""IPC bridge：把 Phase 1 Pipeline 包成 IPC handler（feat-016）。

接收 Swift 端推送的 JPEG 帧流，重建 PIL.Image 组装 Frame，
调 Pipeline.run_frames() 跑字幕提取，回传 entries。

安全措施：
- MAX_FRAMES：帧数上限，防止内存耗尽（每帧 PIL.Image 在内存中）
- MAX_JPEG_BYTES：单帧 JPEG 字节上限，防止解压炸弹
- MAX_IMAGE_PIXELS：PIL.Image.MAX_IMAGE_PIXELS 限制，防止解压后像素爆炸
- JPEG 解码失败优雅降级，不崩溃 server

批量模型（ADR-0007a）：
- start_job → 构造 Pipeline（不构造 Extractor，帧来自 IPC）
- frame → base64 解码 → PIL.Image → 缓冲到列表
- finalize → 调 run_frames → 回传 entries + done
- cancel_job → 清空缓冲，回传 done(ok=False)
"""

from __future__ import annotations

import base64
import io
import logging
from typing import Any

from sublift.config import Config
from sublift.detector.base import Detector
from sublift.detector.bottom_crop import BottomCropDetector
from sublift.detector.fixed_region import FixedRegionDetector
from sublift.ipc.protocol import (
    MSG_CANCEL_JOB,
    MSG_FINALIZE,
    MSG_FRAME,
    MSG_START_JOB,
    ProtocolError,
    build_done,
    build_entries,
    build_error,
    build_progress,
    validate,
)
from sublift.models import BoundingBox, Frame
from sublift.ocr.base import OcrEngine
from sublift.pipeline.core import Pipeline

logger = logging.getLogger(__name__)

# 安全限制
MAX_FRAMES = 60000  # 约 3.3h @ 5fps，硬上限
MAX_JPEG_BYTES = 20 * 1024 * 1024  # 单帧 JPEG 20MB 上限
MAX_TOTAL_PIXELS = 60000 * 1920 * 1080  # 总像素上限（60000 帧 × 1080p）
MAX_IMAGE_PIXELS = 50_000_000  # 单帧像素上限（防 PIL 解压炸弹，约 7000x7000）

# 进度阶段
STAGE_READY = "ready"
STAGE_FRAME_RECEIVED = "frame_received"
STAGE_PROCESSING = "processing"
STAGE_DONE = "done"


def _build_detector(
    region_box: list[int] | None,
    config: Config,
) -> Detector:
    """按 start_job.region_box 选择检测器；无区域时回退下部裁剪。"""
    if region_box is not None:
        x, y, width, height = region_box
        return FixedRegionDetector(BoundingBox(x=x, y=y, width=width, height=height))
    return BottomCropDetector(bottom_ratio=config.region_bottom_ratio)


class BridgeHandler:
    """IPC handler：把 Swift 推送的帧流接入 Pipeline。

    状态机：
        IDLE → start_job → READY → frame* → finalize → DONE
                              ↓ cancel_job → CANCELLED
    """

    def __init__(
        self,
        ocr_engine_factory: type[OcrEngine] | None = None,
        ocr_engine_kwargs: dict[str, Any] | None = None,
    ) -> None:
        """初始化 bridge handler。

        Args:
            ocr_engine_factory: OCR 引擎类（默认 VisionOcrEngine，测试可传 MockOcrEngine）。
            ocr_engine_kwargs: OCR 引擎构造参数。
        """
        if ocr_engine_factory is None:
            from sublift.ocr.vision import VisionOcrEngine

            ocr_engine_factory = VisionOcrEngine
        self._ocr_engine_factory = ocr_engine_factory
        self._ocr_engine_kwargs = ocr_engine_kwargs or {}

        self._pipeline: Pipeline | None = None
        self._frames: list[Frame] = []
        self._total_pixels: int = 0  # 累计像素数，防内存耗尽
        self._video_id: str = ""
        self._fps: float = 5.0
        self._duration_ms: int = 0
        self._est_total_frames: int = 0

    async def handle(self, message: dict[str, Any]) -> dict[str, Any] | None:
        """处理一条 IPC 消息，返回响应或 None（关闭连接）。

        Args:
            message: 已解析的 JSON 字典。

        Returns:
            响应消息字典，或 None 表示关闭连接。
        """
        msg_type = message.get("type")

        if msg_type == "hello":
            from sublift.ipc.protocol import build_bye

            return build_bye()
        if msg_type == "bye":
            return None

        try:
            validate(message)
        except ProtocolError as e:
            return build_error(str(e))

        if msg_type == MSG_START_JOB:
            return self._handle_start_job(message)
        if msg_type == MSG_FRAME:
            return self._handle_frame(message)
        if msg_type == MSG_FINALIZE:
            return await self._handle_finalize(message)
        if msg_type == MSG_CANCEL_JOB:
            return self._handle_cancel(message)

        return build_error(f"unknown type: {msg_type!r}")

    def _handle_start_job(self, message: dict[str, Any]) -> dict[str, Any]:
        """构造 Pipeline，进入 READY 状态。"""
        self._video_id = message["video_id"]
        self._fps = message["fps"]
        engine = message["engine"]
        confidence_threshold = message["confidence_threshold"]
        self._duration_ms = message.get("duration_ms", 0)

        if self._duration_ms > 0 and self._fps > 0:
            self._est_total_frames = int(self._duration_ms / 1000 * self._fps)

        config = Config(
            sample_fps=self._fps,
            confidence_threshold=confidence_threshold,
        )

        try:
            ocr = self._ocr_engine_factory(**self._ocr_engine_kwargs)
        except Exception as e:
            logger.exception("OCR 引擎构造失败")
            return build_done(self._video_id, ok=False, error=str(e))

        detector = _build_detector(message.get("region_box"), config)
        # Pipeline 构造：帧流模式不需要 extractor
        self._pipeline = Pipeline(
            detector=detector,
            ocr=ocr,
            config=config,
        )
        self._frames = []
        self._total_pixels = 0

        logger.info(
            "start_job: video_id=%s fps=%.1f engine=%s est_frames=%d",
            self._video_id,
            self._fps,
            engine,
            self._est_total_frames,
        )
        return build_progress(
            self._video_id, STAGE_READY, 0.0, 0
        )

    def _handle_frame(self, message: dict[str, Any]) -> dict[str, Any]:
        """解码 JPEG → PIL.Image → 缓冲 Frame。"""
        if self._pipeline is None:
            return build_error("frame received before start_job")

        if len(self._frames) >= MAX_FRAMES:
            return build_done(
                self._video_id,
                ok=False,
                error=f"帧数超限: {MAX_FRAMES}",
            )

        video_id = message["video_id"]
        ts_ms = message["ts_ms"]
        jpeg_b64 = message["jpeg_bytes"]

        try:
            jpeg_bytes = base64.b64decode(jpeg_b64, validate=True)
        except Exception:
            return build_error(f"base64 解码失败: ts_ms={ts_ms}")

        if len(jpeg_bytes) > MAX_JPEG_BYTES:
            return build_error(
                f"JPEG 过大: {len(jpeg_bytes)} > {MAX_JPEG_BYTES}"
            )

        try:
            from PIL import Image

            # 设置 PIL 解压炸弹防护
            Image.MAX_IMAGE_PIXELS = MAX_IMAGE_PIXELS
            image = Image.open(io.BytesIO(jpeg_bytes))
            image.load()
        except Image.DecompressionBombError:
            return build_error(f"像素超限（解压炸弹）: ts_ms={ts_ms}")
        except Exception as e:
            return build_error(f"JPEG 解码失败: {e}")

        # 累计像素检查
        width, height = image.size
        frame_pixels = width * height
        if self._total_pixels + frame_pixels > MAX_TOTAL_PIXELS:
            return build_done(
                self._video_id,
                ok=False,
                error=f"总像素超限: {self._total_pixels + frame_pixels} > {MAX_TOTAL_PIXELS}",
            )

        self._frames.append(Frame(timestamp_ms=ts_ms, image=image))
        self._total_pixels += frame_pixels

        pct = 0.0
        if self._est_total_frames > 0:
            pct = min(len(self._frames) / self._est_total_frames, 1.0)

        return build_progress(
            video_id, STAGE_FRAME_RECEIVED, pct, 0
        )

    async def _handle_finalize(self, message: dict[str, Any]) -> dict[str, Any]:
        """调 Pipeline.run_frames，回传 entries + done。"""
        if self._pipeline is None:
            return build_error("finalize received before start_job")

        video_id = message["video_id"]

        # 用 asyncio.to_thread 在线程池里跑 Pipeline，避免阻塞事件循环
        import asyncio

        entries = await asyncio.to_thread(
            self._pipeline.run_frames, iter(self._frames)
        )

        entry_dicts = [
            {
                "start_ms": e.start_ms,
                "end_ms": e.end_ms,
                "text": e.text,
                "confidence": e.confidence,
            }
            for e in entries
        ]

        logger.info(
            "finalize: video_id=%s frames=%d entries=%d",
            video_id,
            len(self._frames),
            len(entries),
        )

        # 一次性回传 entries（批量模型）
        self._frames = []
        self._total_pixels = 0
        self._pipeline = None
        return build_entries(video_id, entry_dicts)

    def _handle_cancel(self, message: dict[str, Any]) -> dict[str, Any]:
        """取消任务，清空缓冲。"""
        video_id = message["video_id"]
        n = len(self._frames)
        self._frames = []
        self._total_pixels = 0
        self._pipeline = None
        logger.info("cancel_job: video_id=%s discarded_frames=%d", video_id, n)
        return build_done(video_id, ok=False, error="cancelled")
