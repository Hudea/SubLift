"""IPC bridge：把 Pipeline 包成 IPC handler（feat-016 / feat-029）。

接收 Swift 端推送的 JPEG 帧流，解码后立即喂给 Pipeline.feed()，段闭合时
OCR 并推送 push_entry 给 Swift（增量显示）。finalize 时跑全局 dedupe，
推送 entries(is_final=True) 全量替换。

流式模型（feat-029）：
- start_job → 构造 Pipeline（不构造 Extractor，帧来自 IPC）
- frame → JPEG 解码 → Pipeline.feed() → 段闭合则 OCR + push_entry → progress
- finalize → Pipeline.finalize() → entries(is_final=True) → done
- cancel_job → Pipeline.cancel() → done(ok=False)

安全措施：
- MAX_JPEG_BYTES：单帧 JPEG 字节上限，防止解压炸弹
- MAX_IMAGE_PIXELS：PIL.Image.MAX_IMAGE_PIXELS 限制，防止解压后像素爆炸
- JPEG 解码失败优雅降级，不崩溃 server
"""

from __future__ import annotations

import base64
import io
import logging
from typing import Any

from sublift.config import ChangePointConfig, Config
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
    build_push_entry,
    validate,
)
from sublift.ipc.server import PushCallback
from sublift.models import BoundingBox, Frame
from sublift.ocr.base import OcrEngine
from sublift.pipeline.core import Pipeline

logger = logging.getLogger(__name__)

# 安全限制（feat-029：不再需要 MAX_FRAMES / MAX_TOTAL_PIXELS，因为不攒帧）
MAX_JPEG_BYTES = 20 * 1024 * 1024  # 单帧 JPEG 20MB 上限
MAX_IMAGE_PIXELS = 50_000_000  # 单帧像素上限（防 PIL 解压炸弹，约 7000x7000）

# 进度阶段
STAGE_READY = "ready"
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
    """IPC handler：把 Swift 推送的帧流接入 Pipeline（流式模式）。

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
        self._video_id: str = ""
        self._fps: float = 5.0
        self._duration_ms: int = 0
        self._est_total_frames: int = 0
        self._cancelled: bool = False

    async def handle(
        self,
        message: dict[str, Any],
        push: PushCallback,
    ) -> dict[str, Any] | None:
        """处理一条 IPC 消息，返回响应或 None（关闭连接）。

        Args:
            message: 已解析的 JSON 字典。
            push: 增量推送回调，可在返回主响应前推送 push_entry 等消息。

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
            return await self._handle_frame(message, push)
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

        cp_config = ChangePointConfig()
        if "enable_ssim_patrol" in message and message["enable_ssim_patrol"] is not None:
            cp_config = ChangePointConfig(
                enable_ssim_patrol=bool(message["enable_ssim_patrol"])
            )

        config = Config(
            sample_fps=self._fps,
            confidence_threshold=confidence_threshold,
            change_point=cp_config,
        )

        try:
            ocr = self._ocr_engine_factory(**self._ocr_engine_kwargs)
        except Exception as e:
            logger.exception("OCR 引擎构造失败")
            return build_done(self._video_id, ok=False, error=str(e))

        detector = _build_detector(message.get("region_box"), config)
        self._pipeline = Pipeline(
            detector=detector,
            ocr=ocr,
            config=config,
        )
        self._cancelled = False

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

    async def _handle_frame(
        self,
        message: dict[str, Any],
        push: PushCallback,
    ) -> dict[str, Any]:
        """解码 JPEG → Pipeline.feed() → 段闭合时 OCR + push_entry → progress。"""
        if self._pipeline is None:
            return build_error("frame received before start_job")

        if self._cancelled:
            return build_error("job cancelled")

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

            Image.MAX_IMAGE_PIXELS = MAX_IMAGE_PIXELS
            image = Image.open(io.BytesIO(jpeg_bytes))
            image.load()
        except Image.DecompressionBombError:
            return build_error(f"像素超限（解压炸弹）: ts_ms={ts_ms}")
        except Exception as e:
            return build_error(f"JPEG 解码失败: {e}")

        frame = Frame(timestamp_ms=ts_ms, image=image)

        # 流式推进打轴（<1ms）
        event = self._pipeline.feed(frame)

        # 段闭合 → OCR（放线程池避免阻塞 event loop）→ push_entry
        if event is not None:
            import asyncio

            entry = await asyncio.to_thread(self._pipeline.ocr_segment, event)
            await push(build_push_entry(video_id, {
                "start_ms": entry.start_ms,
                "end_ms": entry.end_ms,
                "text": entry.text,
                "confidence": entry.confidence,
            }))

        # 进度
        pct = 0.0
        if self._est_total_frames > 0:
            pct = min(self._pipeline.processed_count / self._est_total_frames, 1.0)

        return build_progress(
            video_id, STAGE_PROCESSING, pct, 0
        )

    async def _handle_finalize(self, message: dict[str, Any]) -> dict[str, Any]:
        """Pipeline.finalize() → entries(is_final=True) → done。"""
        if self._pipeline is None:
            return build_error("finalize received before start_job")

        video_id = message["video_id"]

        import asyncio

        entries = await asyncio.to_thread(self._pipeline.finalize)

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
            "finalize: video_id=%s processed_frames=%d entries=%d",
            video_id,
            self._pipeline.processed_count,
            len(entries),
        )

        self._pipeline = None
        return build_entries(video_id, entry_dicts, is_final=True)

    def _handle_cancel(self, message: dict[str, Any]) -> dict[str, Any]:
        """取消任务，清理 Pipeline 状态。"""
        video_id = message["video_id"]
        self._cancelled = True
        if self._pipeline is not None:
            self._pipeline.cancel()
            self._pipeline = None
        logger.info("cancel_job: video_id=%s", video_id)
        return build_done(video_id, ok=False, error="cancelled")
