"""IPC bridge：把 Pipeline 包成 IPC handler（feat-016 / feat-029 / 统一抽帧）。

两种模式：

1. **path mode**（GUI 默认）：``start_job.video_path`` 非空 → 后端
   ``FfmpegExtractor`` 自抽帧（与 CLI/benchmark 同源，无 JPEG），
   流式 feed + OCR，推送 progress/push_entry，主响应 ``entries(is_final=True)``。
2. **frame mode**（兼容/调试）：无 ``video_path`` → Swift 推 JPEG frame 流，
   finalize 时闭合。

流式模型（feat-029）：
- start_job → 构造 Pipeline；path mode 则立即跑完整提取
- frame → JPEG 解码 → Pipeline.feed()（仅 frame mode）
- finalize → Pipeline.finalize()（仅 frame mode）
- cancel_job → Pipeline.cancel()

安全措施：
- MAX_JPEG_BYTES：单帧 JPEG 字节上限，防止解压炸弹
- MAX_IMAGE_PIXELS：PIL.Image.MAX_IMAGE_PIXELS 限制，防止解压后像素爆炸
- JPEG 解码失败优雅降级，不崩溃 server
"""

from __future__ import annotations

import asyncio
import base64
import io
import logging
import queue
import threading
from pathlib import Path
from typing import Any

from sublift.config import ChangePointConfig, Config
from sublift.detector.base import Detector
from sublift.detector.bottom_crop import BottomCropDetector
from sublift.detector.fixed_region import FixedRegionDetector
from sublift.extractor.ffmpeg_extractor import FfmpegExtractor
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

# path mode 进度推送间隔（帧），避免每帧写 socket
_PROGRESS_EVERY_N_FRAMES = 5


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
    """IPC handler：path mode（后端 ffmpeg）或 frame mode（Swift 推帧）。

    状态机：
        IDLE → start_job(frame) → READY → frame* → finalize → DONE
        IDLE → start_job(path)  → 内部抽帧+处理 → DONE（主响应 entries）
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
        self._path_mode: bool = False

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
            return await self._handle_start_job(message, push)
        if msg_type == MSG_FRAME:
            return await self._handle_frame(message, push)
        if msg_type == MSG_FINALIZE:
            return await self._handle_finalize(message)
        if msg_type == MSG_CANCEL_JOB:
            return self._handle_cancel(message)

        return build_error(f"unknown type: {msg_type!r}")

    async def _handle_start_job(
        self,
        message: dict[str, Any],
        push: PushCallback,
    ) -> dict[str, Any]:
        """构造 Pipeline；path mode 则后端 ffmpeg 完整提取。"""
        setup = self._setup_pipeline(message)
        if setup is not None:
            return setup  # done(ok=False) 等错误

        video_path_raw = message.get("video_path")
        if isinstance(video_path_raw, str) and video_path_raw.strip():
            self._path_mode = True
            return await self._run_path_mode(video_path_raw.strip(), push)

        self._path_mode = False
        return build_progress(self._video_id, STAGE_READY, 0.0, 0)

    def _setup_pipeline(self, message: dict[str, Any]) -> dict[str, Any] | None:
        """根据 start_job 构造 Pipeline。成功返回 None，失败返回 done/error。"""
        self._video_id = message["video_id"]
        self._fps = float(message["fps"])
        engine = message["engine"]
        confidence_threshold = float(message["confidence_threshold"])
        self._duration_ms = int(message.get("duration_ms", 0) or 0)

        if self._duration_ms > 0 and self._fps > 0:
            self._est_total_frames = int(self._duration_ms / 1000 * self._fps)
        else:
            self._est_total_frames = 0

        raw_region = message.get("region_box")
        patrol_msg = message.get("enable_ssim_patrol")
        video_path_log = message.get("video_path")
        raw_profile = message.get("subtitle_profile")

        cp_config = ChangePointConfig()
        if "enable_ssim_patrol" in message and message["enable_ssim_patrol"] is not None:
            cp_config = ChangePointConfig(
                enable_ssim_patrol=bool(message["enable_ssim_patrol"])
            )

        subtitle_profile = None
        if isinstance(raw_profile, dict):
            from sublift.models import SubtitleProfile

            try:
                subtitle_profile = SubtitleProfile.from_dict(raw_profile)
            except ValueError as e:
                logger.exception("subtitle_profile 解析失败")
                return build_done(self._video_id, ok=False, error=str(e))
        elif raw_profile is None and isinstance(raw_region, list) and len(raw_region) == 4:
            # 无显式 profile 时从 region_box 推导 crop 全带默认（CLI 同源约定）
            from sublift.models import SubtitleProfile

            _x, _y, rw, rh = (int(v) for v in raw_region)
            subtitle_profile = SubtitleProfile.from_crop(rw, rh)

        config = Config(
            sample_fps=self._fps,
            confidence_threshold=confidence_threshold,
            change_point=cp_config,
            subtitle_profile=subtitle_profile,
        )

        try:
            ocr = self._ocr_engine_factory(**self._ocr_engine_kwargs)
        except Exception as e:
            logger.exception("OCR 引擎构造失败")
            return build_done(self._video_id, ok=False, error=str(e))

        detector = _build_detector(raw_region, config)
        detector_name = type(detector).__name__
        self._pipeline = Pipeline(
            detector=detector,
            ocr=ocr,
            config=config,
        )
        self._cancelled = False

        logger.info(
            "start_job received: video_id=%s fps=%.1f engine=%s "
            "confidence_threshold=%.3f duration_ms=%d est_frames=%d "
            "region_box=%s enable_ssim_patrol_msg=%s video_path=%s "
            "subtitle_profile=%s",
            self._video_id,
            self._fps,
            engine,
            confidence_threshold,
            self._duration_ms,
            self._est_total_frames,
            raw_region,
            patrol_msg,
            video_path_log,
            raw_profile,
        )
        logger.info(
            "start_job effective: detector=%s sample_fps=%.1f conf=%.3f "
            "hysteresis_frames=%d ocr_anchor_delay_frames=%d drop_empty_text=%s "
            "enable_ssim_patrol=%s presence_threshold=%.4f change_threshold=%d "
            "min_duration_ms=%d merge_gap_ms=%d subtitle_profile=%s",
            detector_name,
            config.sample_fps,
            config.confidence_threshold,
            config.change_point.hysteresis_frames,
            config.ocr_anchor_delay_frames,
            config.drop_empty_text,
            config.change_point.enable_ssim_patrol,
            config.change_point.presence_threshold,
            config.change_point.change_threshold,
            config.min_duration_ms,
            config.merge_gap_ms,
            config.subtitle_profile.to_dict() if config.subtitle_profile else None,
        )
        if (
            isinstance(detector, FixedRegionDetector)
            and isinstance(raw_region, list)
            and len(raw_region) == 4
        ):
            x = int(raw_region[0])
            y = int(raw_region[1])
            w = int(raw_region[2])
            h = int(raw_region[3])
            feat033 = [0, 848, 1920, 87]
            match = [x, y, w, h] == feat033
            logger.info(
                "start_job fixed_region box: [%d, %d, %d, %d] "
                "matches_feat033_%s=%s",
                x,
                y,
                w,
                h,
                feat033,
                match,
            )
        return None

    async def _run_path_mode(
        self,
        video_path: str,
        push: PushCallback,
    ) -> dict[str, Any]:
        """后端 FfmpegExtractor 抽帧并跑完整 pipeline（与 CLI/benchmark 同源）。

        整段 extract+OCR 放在**单一工作线程**中执行（避免 generator 跨线程 next 卡死），
        进度通过共享计数 + asyncio 心跳推送。
        """
        assert self._pipeline is not None
        path = Path(video_path)
        if not path.is_file():
            self._pipeline = None
            self._path_mode = False
            return build_done(
                self._video_id, ok=False, error=f"视频文件不存在: {video_path}"
            )

        video_id = self._video_id
        await push(build_progress(video_id, STAGE_READY, 0.0, 0))
        await push(build_progress(video_id, STAGE_PROCESSING, 0.0, 0))
        logger.info(
            "path_mode extract: video_id=%s path=%s fps=%.1f",
            video_id,
            path,
            self._fps,
        )

        pipeline = self._pipeline
        est = max(self._est_total_frames, 1)
        msg_q: queue.Queue[tuple[str, Any]] = queue.Queue()

        def _worker() -> None:
            try:
                extractor = FfmpegExtractor(fps=self._fps)
                logger.info("path_mode worker: starting extract loop")
                n = 0
                for frame in extractor.extract(path):
                    if self._cancelled:
                        msg_q.put(("cancel", None))
                        return
                    n += 1
                    if n == 1:
                        logger.info(
                            "path_mode first_frame: ts_ms=%d", frame.timestamp_ms
                        )
                    # 首帧 / 每 N 帧推送进度，避免每帧写 UDS 与 Swift MainActor 积压
                    if n == 1 or n % _PROGRESS_EVERY_N_FRAMES == 0:
                        msg_q.put(("progress", n))
                    event = pipeline.feed(frame)
                    if event is not None:
                        entry = pipeline.ocr_segment(event)
                        msg_q.put(("entry", entry))
                if self._cancelled:
                    msg_q.put(("cancel", None))
                    return
                logger.info("path_mode frames_done: n=%d, finalizing…", n)
                entries = pipeline.finalize()
                msg_q.put(("entries", entries))
            except BaseException as exc:
                logger.exception("path_mode worker failed")
                msg_q.put(("error", exc))

        thread = threading.Thread(target=_worker, name="path-mode-extract", daemon=True)
        thread.start()

        entries = None
        try:
            while True:
                if self._cancelled and not thread.is_alive():
                    self._pipeline = None
                    self._path_mode = False
                    return build_done(video_id, ok=False, error="cancelled")

                try:
                    kind, payload = msg_q.get(timeout=0.2)
                except queue.Empty:
                    if not thread.is_alive() and msg_q.empty():
                        break
                    continue

                if kind == "progress":
                    n = int(payload)
                    pct = min(n / est, 0.99)
                    await push(build_progress(video_id, STAGE_PROCESSING, pct, 0))
                elif kind == "entry":
                    entry = payload
                    await push(
                        build_push_entry(
                            video_id,
                            {
                                "start_ms": entry.start_ms,
                                "end_ms": entry.end_ms,
                                "text": entry.text,
                                "confidence": entry.confidence,
                            },
                        )
                    )
                elif kind == "entries":
                    entries = payload
                    break
                elif kind == "cancel":
                    self._pipeline = None
                    self._path_mode = False
                    return build_done(video_id, ok=False, error="cancelled")
                elif kind == "error":
                    self._pipeline = None
                    self._path_mode = False
                    return build_done(video_id, ok=False, error=str(payload))
        except Exception as e:
            logger.exception("path_mode 提取失败")
            self._cancelled = True
            self._pipeline = None
            self._path_mode = False
            return build_done(video_id, ok=False, error=str(e))
        finally:
            thread.join(timeout=5.0)

        if entries is None:
            self._pipeline = None
            self._path_mode = False
            return build_done(video_id, ok=False, error="path_mode 未产出 entries")

        entry_dicts = [
            {
                "start_ms": e.start_ms,
                "end_ms": e.end_ms,
                "text": e.text,
                "confidence": e.confidence,
            }
            for e in entries
        ]
        empty_n = sum(1 for e in entries if not e.text.strip())
        logger.info(
            "path_mode done: video_id=%s entries=%d empty_text=%d",
            video_id,
            len(entries),
            empty_n,
        )
        await push(build_progress(video_id, STAGE_PROCESSING, 1.0, 0))
        self._pipeline = None
        self._path_mode = False
        return build_entries(video_id, entry_dicts, is_final=True)

    async def _handle_frame(
        self,
        message: dict[str, Any],
        push: PushCallback,
    ) -> dict[str, Any]:
        """解码 JPEG → Pipeline.feed() → 段闭合时 OCR + push_entry → progress。"""
        if self._path_mode:
            return build_error("path mode 不接受 frame；抽帧由后端 FfmpegExtractor 完成")
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
        """Pipeline.finalize() → entries(is_final=True)（仅 frame mode）。"""
        if self._path_mode:
            return build_error("path mode 不需要 finalize；start_job 已返回完整 entries")
        if self._pipeline is None:
            return build_error("finalize received before start_job")

        video_id = message["video_id"]

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

        empty_n = sum(1 for e in entries if not e.text.strip())
        logger.info(
            "finalize: video_id=%s processed_frames=%d entries=%d empty_text=%d",
            video_id,
            self._pipeline.processed_count,
            len(entries),
            empty_n,
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
        self._path_mode = False
        logger.info("cancel_job: video_id=%s", video_id)
        return build_done(video_id, ok=False, error="cancelled")
