"""端到端编排：detector → signature → changepoint → timeline → ocr → dedupe。

Pipeline 串联七个组件，支持两种使用模式：

1. **批量模式**（向后兼容）：``run()`` / ``run_frames()`` 一次性处理全部帧，
   返回完整字幕列表。
2. **流式模式**（feat-029）：``feed()`` 逐帧推进打轴，段闭合时返回
   :class:`SegmentEvent`；调用方按需 ``ocr_segment()``，最终 ``finalize()``
   跑全局 dedupe 返回最终列表。

流式模式让 bridge 边收帧边处理，首条字幕反馈时间从「全片处理完」降到
「第一段闭合 + 一次 OCR」，内存从「全部帧」降到「1 帧 + N 个 anchor」。
"""

from __future__ import annotations

from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING

import cv2
import numpy as np

from sublift.config import DEFAULT_CONFIG, Config
from sublift.detector.base import Detector
from sublift.extractor.base import Extractor
from sublift.models import BoundingBox, Frame, Region, SubtitleEntry
from sublift.ocr.base import OcrEngine
from sublift.pipeline.changepoint import ChangePointDetector, EventType
from sublift.pipeline.dedupe import merge_entries
from sublift.pipeline.signature import compute_signature
from sublift.pipeline.timeline import TimelineBuilder

if TYPE_CHECKING:
    from PIL import Image

    from sublift.diagnostics.trace import TraceRecorder


@dataclass(frozen=True)
class SegmentEvent:
    """段闭合事件。

    由 :meth:`Pipeline.feed` 在段闭合（OUT/CHANGE）时返回，携带 OCR
    代表帧供 :meth:`Pipeline.ocr_segment` 使用。

    Attributes:
        start_ms: 段起始时间戳（毫秒）。
        end_ms: 段结束时间戳（毫秒）。
        anchor_frame: 主 OCR 帧（延迟锚或稳定帧）；None 时 OCR 空文本。
        fallback_frames: 额外候选帧（主帧失败时按序重试，feat-033b）。
    """

    start_ms: int
    end_ms: int
    anchor_frame: Frame | None
    fallback_frames: tuple[Frame, ...] = ()


class Pipeline:
    """字幕提取流水线编排器。

    组合 detector/ocr，内部串联 signature/changepoint/timeline/dedupe。

    两种使用模式：

    批量模式（向后兼容）::

        pipeline = Pipeline(detector, ocr, config, extractor=ext)
        entries = pipeline.run(video_path)

    流式模式（feat-029）::

        pipeline = Pipeline(detector, ocr, config)
        for frame in stream:
            event = pipeline.feed(frame)
            if event is not None:
                entry = pipeline.ocr_segment(event)
                # 增量推送 entry 给 UI
        final = pipeline.finalize()  # 关闭末段 + dedupe
    """

    def __init__(
        self,
        detector: Detector,
        ocr: OcrEngine,
        config: Config = DEFAULT_CONFIG,
        *,
        extractor: Extractor | None = None,
        trace_recorder: TraceRecorder | None = None,
    ) -> None:
        """初始化流水线。

        Args:
            detector: 字幕区域检测器（首帧一次性确定 Region）。
            ocr: OCR 引擎（每段代表帧调用一次）。
            config: 提取流程配置。
            extractor: 帧采样器，文件模式必填（run() 调用）。
                帧流模式（run_frames()/feed()）不需要，可不传。
            trace_recorder: 可选打轴决策 trace 记录器（feat-031a）。
                注入后 ``ChangePointDetector`` 会逐帧记录决策上下文。
        """
        self._extractor = extractor
        self._detector = detector
        self._ocr = ocr
        self._config = config
        self._trace_recorder = trace_recorder

        # 流式状态（feed/ocr_segment/finalize 共享）
        self._changepoint = ChangePointDetector(
            config=self._config.change_point,
            trace_recorder=self._trace_recorder,
        )
        self._timeline = TimelineBuilder()
        self._region: Region | None = None
        self._anchor_frames: dict[int, Frame] = {}
        self._closed_entries: list[SubtitleEntry] = []
        self._open_segment_start_ms: int | None = None
        self._processed_count = 0
        self._last_timestamp_ms = 0
        self._cancelled = False
        # feat-033b：OCR 锚帧延迟 + 稳定帧/首帧回退
        self._pending_anchor_remaining: int = 0
        self._segment_first_frame: Frame | None = None
        self._segment_stable_frame: Frame | None = None

    # ------------------------------------------------------------------
    # 流式 API（feat-029）
    # ------------------------------------------------------------------

    def feed(self, frame: Frame) -> SegmentEvent | None:
        """推进一帧打轴。

        轻量同步（<1ms）：仅做 crop + signature + changepoint。
        段闭合（OUT/CHANGE）时返回 :class:`SegmentEvent`，调用方应随后调用
        :meth:`ocr_segment` 获取该段 OCR 文本。

        Args:
            frame: 视频帧。

        Returns:
            段闭合事件；未闭合时返回 None。取消后返回 None 且不处理。
        """
        if self._cancelled:
            return None

        self._last_timestamp_ms = frame.timestamp_ms
        self._processed_count += 1

        # 首帧检测字幕区域
        if self._region is None:
            self._region = self._detector.detect(frame)
            if self._region is None:
                return None

        crop_image = self._crop_region(frame, self._region.box)
        crop_np = cv2.cvtColor(np.asarray(crop_image), cv2.COLOR_RGB2BGR)

        signature = compute_signature(
            crop_np, frame.timestamp_ms, self._config.signature
        )
        event = self._changepoint.process(signature, crop_np)

        if event is None:
            self._on_stable_frame(frame)
            return None

        if event.event_type == EventType.IN:
            self._open_segment(event.timestamp_ms, frame)
            self._timeline.consume(event)
            return None

        if event.event_type == EventType.OUT:
            start_ms = self._open_segment_start_ms
            self._timeline.consume(event)
            seg_event = self._close_segment(
                start_ms=start_ms if start_ms is not None else event.timestamp_ms,
                end_ms=event.timestamp_ms,
                closing_frame=frame,
            )
            return seg_event

        # CHANGE：旧段闭合 + 新段开启
        old_start = self._open_segment_start_ms
        self._timeline.consume(event)
        end_ms = event.prev_end_ms if event.prev_end_ms is not None else event.timestamp_ms
        # 闭合旧段时用稳定帧（本帧已是新字幕，勿作旧段 OCR 主锚）
        closed = self._close_segment(
            start_ms=old_start if old_start is not None else event.timestamp_ms,
            end_ms=end_ms,
            closing_frame=None,
        )
        self._open_segment(event.timestamp_ms, frame)
        return closed

    def ocr_segment(self, event: SegmentEvent) -> SubtitleEntry:
        """OCR 一个已闭合的段并缓存 raw entry。

        重操作（~几百 ms）：调用方应放到线程池，避免阻塞事件循环。
        OCR 完成后 raw entry 缓存到内部列表，供 :meth:`finalize` dedupe。

        主锚帧置信不足或空文本时，尝试 ``fallback_frame``（feat-033b）。

        Args:
            event: :meth:`feed` 返回的段闭合事件。

        Returns:
            带 OCR 文本的 SubtitleEntry（未经 dedupe）。
        """
        if self._region is None:
            entry = SubtitleEntry(
                start_ms=event.start_ms,
                end_ms=event.end_ms,
                text="",
                confidence=0.0,
            )
            self._closed_entries.append(entry)
            return entry

        text, confidence = self._ocr_frame(event.anchor_frame)
        if self._needs_ocr_retry(text, confidence):
            seen_ts = {
                event.anchor_frame.timestamp_ms
                if event.anchor_frame is not None
                else -1
            }
            for fb in event.fallback_frames:
                if fb.timestamp_ms in seen_ts:
                    continue
                seen_ts.add(fb.timestamp_ms)
                fb_text, fb_conf = self._ocr_frame(fb)
                if not self._needs_ocr_retry(fb_text, fb_conf):
                    text, confidence = fb_text, fb_conf
                    break
                if fb_text.strip() and not text.strip():
                    text, confidence = fb_text, fb_conf
                if fb_conf > confidence and fb_text.strip():
                    text, confidence = fb_text, fb_conf

        if confidence < self._config.confidence_threshold:
            text = ""

        entry = SubtitleEntry(
            start_ms=event.start_ms,
            end_ms=event.end_ms,
            text=text,
            confidence=confidence,
        )
        self._closed_entries.append(entry)
        return entry

    def finalize(self) -> list[SubtitleEntry]:
        """关闭末尾未闭合段（如有），OCR 之，全局 dedupe 返回最终列表。

        Returns:
            去重合并后的字幕条目列表。
        """
        # 关闭末尾未闭合段
        if self._open_segment_start_ms is not None:
            self._timeline.finalize_open_segment(self._last_timestamp_ms)
            segments = self._timeline.build()
            if segments:
                last = segments[-1]
                if last.end_ms is not None:
                    event = self._close_segment(
                        start_ms=last.start_ms,
                        end_ms=last.end_ms,
                        closing_frame=None,
                        clear_open=False,
                    )
                    self.ocr_segment(event)
            self._open_segment_start_ms = None
            self._reset_segment_ocr_state()

        return merge_entries(
            self._closed_entries,
            merge_gap_ms=self._config.merge_gap_ms,
            min_duration_ms=self._config.min_duration_ms,
            drop_empty_text=self._config.drop_empty_text,
        )

    def cancel(self) -> None:
        """取消并清理内部状态，释放内存。后续 feed() 将拒绝处理。"""
        self._cancelled = True
        self._changepoint.reset()
        self._timeline.reset()
        self._region = None
        self._anchor_frames.clear()
        self._closed_entries.clear()
        self._open_segment_start_ms = None
        self._processed_count = 0
        self._last_timestamp_ms = 0
        self._reset_segment_ocr_state()

    def _open_segment(self, start_ms: int, frame: Frame) -> None:
        """开启新段并启动 OCR 锚帧延迟（feat-033b）。"""
        self._open_segment_start_ms = start_ms
        self._segment_first_frame = frame
        self._segment_stable_frame = None
        delay = max(0, self._config.ocr_anchor_delay_frames)
        if delay == 0:
            self._anchor_frames[start_ms] = frame
            self._pending_anchor_remaining = 0
        else:
            self._pending_anchor_remaining = delay

    def _on_stable_frame(self, frame: Frame) -> None:
        """无状态事件时：推进延迟锚 / 更新稳定帧。"""
        if self._open_segment_start_ms is None:
            return
        if self._pending_anchor_remaining > 0:
            self._pending_anchor_remaining -= 1
            if self._pending_anchor_remaining == 0:
                self._anchor_frames[self._open_segment_start_ms] = frame
            return
        self._segment_stable_frame = frame

    def _close_segment(
        self,
        *,
        start_ms: int,
        end_ms: int,
        closing_frame: Frame | None,
        clear_open: bool = True,
    ) -> SegmentEvent:
        """选取 OCR 主锚与回退帧，并清理段状态。"""
        delayed = self._anchor_frames.pop(start_ms, None)
        first = self._segment_first_frame
        stable = self._segment_stable_frame
        # 主锚：延迟锁定帧 > 稳定帧 > 段首帧
        anchor = delayed or stable or first
        # 回退候选：稳定帧 / 段首 / 闭合帧（去重，按可读性优先级）
        fallbacks: list[Frame] = []
        seen: set[int] = set()
        if anchor is not None:
            seen.add(anchor.timestamp_ms)
        for cand in (stable, first, closing_frame, delayed):
            if cand is None or cand.timestamp_ms in seen:
                continue
            seen.add(cand.timestamp_ms)
            fallbacks.append(cand)

        if clear_open:
            self._open_segment_start_ms = None
            self._reset_segment_ocr_state()

        return SegmentEvent(
            start_ms=start_ms,
            end_ms=end_ms,
            anchor_frame=anchor,
            fallback_frames=tuple(fallbacks),
        )

    def _reset_segment_ocr_state(self) -> None:
        self._pending_anchor_remaining = 0
        self._segment_first_frame = None
        self._segment_stable_frame = None

    def _ocr_frame(self, frame: Frame | None) -> tuple[str, float]:
        if frame is None or self._region is None:
            return "", 0.0
        crop_image = self._crop_region(frame, self._region.box)
        result = self._ocr.recognize(crop_image)
        return result.text, result.confidence

    def _needs_ocr_retry(self, text: str, confidence: float) -> bool:
        if not text.strip():
            return True
        return confidence < self._config.confidence_threshold

    @property
    def processed_count(self) -> int:
        """已处理帧数。"""
        return self._processed_count

    # ------------------------------------------------------------------
    # 批量 API（向后兼容）
    # ------------------------------------------------------------------

    def run(self, video_path: Path) -> list[SubtitleEntry]:
        """执行端到端字幕提取（文件模式）。

        Args:
            video_path: 视频文件路径。

        Returns:
            清理后的字幕条目列表。

        Raises:
            RuntimeError: 未传 extractor。
        """
        if self._extractor is None:
            raise RuntimeError("文件模式 run() 需要 extractor，请在构造 Pipeline 时传入")
        frames = self._extractor.extract(video_path)
        return self.run_frames(frames)

    def run_frames(self, frames: Iterator[Frame]) -> list[SubtitleEntry]:
        """执行端到端字幕提取（帧流模式，批量向后兼容）。

        内部走 feed + ocr_segment + finalize 流式路径，结果与旧实现等价。

        Args:
            frames: 帧迭代器。

        Returns:
            清理后的字幕条目列表。
        """
        self._reset_streaming_state()
        for frame in frames:
            event = self.feed(frame)
            if event is not None:
                self.ocr_segment(event)
        return self.finalize()

    # ------------------------------------------------------------------
    # 内部辅助
    # ------------------------------------------------------------------

    def _reset_streaming_state(self) -> None:
        """重置流式状态（feed/ocr_segment/finalize 共享）。"""
        self._changepoint = ChangePointDetector(
            config=self._config.change_point,
            trace_recorder=self._trace_recorder,
        )
        self._timeline = TimelineBuilder()
        self._region = None
        self._anchor_frames = {}
        self._closed_entries = []
        self._open_segment_start_ms = None
        self._processed_count = 0
        self._last_timestamp_ms = 0
        self._cancelled = False

    @staticmethod
    def _crop_region(frame: Frame, box: BoundingBox) -> Image.Image:
        """从帧裁剪字幕区域。"""
        return frame.image.crop(
            (box.x, box.y, box.x + box.width, box.y + box.height)
        )
