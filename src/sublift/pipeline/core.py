"""端到端编排：extractor → detector → signature → changepoint → timeline → ocr → dedupe。

Pipeline 串联七个组件，输入 video_path 或帧流，输出 list[SubtitleEntry]。
OCR 后置：timeline 全部构建后，每段只调一次 OCR（段首代表帧）。
"""

from __future__ import annotations

from collections.abc import Iterator
from pathlib import Path
from typing import TYPE_CHECKING

import cv2
import numpy as np

from sublift.config import DEFAULT_CONFIG, Config
from sublift.detector.base import Detector
from sublift.extractor.base import Extractor
from sublift.models import BoundingBox, Frame, SubtitleEntry
from sublift.ocr.base import OcrEngine
from sublift.pipeline.changepoint import ChangePointDetector, EventType
from sublift.pipeline.dedupe import merge_entries
from sublift.pipeline.signature import compute_signature
from sublift.pipeline.timeline import TimelineBuilder, TimelineSegment

if TYPE_CHECKING:
    from PIL import Image


class Pipeline:
    """字幕提取流水线编排器。

    组合 extractor/detector/ocr，内部串联 signature/changepoint/timeline/dedupe，
    产出 SubtitleEntry 列表。
    """

    def __init__(
        self,
        detector: Detector,
        ocr: OcrEngine,
        config: Config = DEFAULT_CONFIG,
        *,
        extractor: Extractor | None = None,
    ) -> None:
        """初始化流水线。

        Args:
            detector: 字幕区域检测器（首帧一次性确定 Region）。
            ocr: OCR 引擎（每段代表帧调用一次）。
            config: 提取流程配置。
            extractor: 帧采样器，文件模式必填（run() 调用）。
                帧流模式（run_frames()）不需要，可不传。
        """
        self._extractor = extractor
        self._detector = detector
        self._ocr = ocr
        self._config = config

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
        """执行端到端字幕提取（帧流模式，ADR-0007a）。

        与 run() 共享内部编排逻辑，但帧来源是外部迭代器而非 extractor。
        用于 IPC bridge（Swift 端推送 JPEG 帧）。

        Args:
            frames: 帧迭代器。

        Returns:
            清理后的字幕条目列表。
        """
        region = None
        changepoint = ChangePointDetector(config=self._config.change_point)
        builder = TimelineBuilder()
        anchor_frames: dict[int, Frame] = {}
        last_timestamp_ms = 0

        for frame in frames:
            last_timestamp_ms = frame.timestamp_ms

            if region is None:
                region = self._detector.detect(frame)

            crop_image = self._crop_region(frame, region.box)
            crop_np = cv2.cvtColor(np.asarray(crop_image), cv2.COLOR_RGB2BGR)

            signature = compute_signature(
                crop_np, frame.timestamp_ms, self._config.signature
            )
            event = changepoint.process(signature, crop_np)

            if event is not None:
                builder.consume(event)
                if event.event_type in (EventType.IN, EventType.CHANGE):
                    anchor_frames[event.timestamp_ms] = frame

        builder.finalize_open_segment(last_timestamp_ms)
        segments = builder.build()

        if region is None:
            return []

        raw_entries = self._ocr_segments(segments, anchor_frames, region.box)
        return merge_entries(
            raw_entries,
            merge_gap_ms=self._config.merge_gap_ms,
            min_duration_ms=self._config.min_duration_ms,
        )

    def _ocr_segments(
        self,
        segments: list[TimelineSegment],
        anchor_frames: dict[int, Frame],
        region_box: BoundingBox,
    ) -> list[SubtitleEntry]:
        """对每个时间轴段调用 OCR，产出 SubtitleEntry 列表。

        Args:
            segments: TimelineSegment 列表。
            anchor_frames: 事件时间戳 → 代表帧的映射。
            region_box: 字幕区域 BoundingBox。

        Returns:
            带 OCR 文本的 SubtitleEntry 列表（未去重）。
        """
        entries: list[SubtitleEntry] = []
        for seg in segments:
            frame = anchor_frames.get(seg.start_ms)
            if frame is None:
                entries.append(
                    SubtitleEntry(
                        start_ms=seg.start_ms,
                        end_ms=seg.end_ms if seg.end_ms is not None else seg.start_ms,
                        text="",
                        confidence=0.0,
                    )
                )
                continue

            crop_image = self._crop_region(frame, region_box)
            ocr_result = self._ocr.recognize(crop_image)

            text = ocr_result.text
            confidence = ocr_result.confidence
            if ocr_result.confidence < self._config.confidence_threshold:
                text = ""

            end_ms = seg.end_ms if seg.end_ms is not None else seg.start_ms
            entries.append(
                SubtitleEntry(
                    start_ms=seg.start_ms,
                    end_ms=end_ms,
                    text=text,
                    confidence=confidence,
                )
            )

        return entries

    @staticmethod
    def _crop_region(frame: Frame, box: BoundingBox) -> Image.Image:
        """从帧裁剪字幕区域。"""
        return frame.image.crop(
            (box.x, box.y, box.x + box.width, box.y + box.height)
        )
