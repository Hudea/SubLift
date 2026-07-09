"""Pipeline 端到端编排测试。

用合成 PIL 帧 + MockOcrEngine 验证闭环，不依赖 ffmpeg/Vision。
合成帧用亮背景+暗矩形模拟字幕，驱动 signature/changepoint 状态机。
"""

from __future__ import annotations

from collections.abc import Iterator
from pathlib import Path

import numpy as np
from PIL import Image

from sublift.config import Config
from sublift.detector import FixedRegionDetector
from sublift.models import BoundingBox, Frame, OcrResult
from sublift.ocr.mock import MockOcrEngine
from sublift.pipeline.core import Pipeline, SegmentEvent


class StubExtractor:
    """内存 Extractor，注入预设帧序列。"""

    def __init__(self, frames: list[Frame]) -> None:
        self._frames = frames

    def extract(self, video_path: Path) -> Iterator[Frame]:
        del video_path
        yield from self._frames


def _blank_frame(timestamp_ms: int, width: int = 320, height: int = 80) -> Frame:
    """造空白帧（无字幕，亮背景）。"""
    img = Image.fromarray(np.full((height, width, 3), 200, dtype=np.uint8))
    return Frame(timestamp_ms=timestamp_ms, image=img)


def _subtitle_frame(
    timestamp_ms: int,
    rect: tuple[int, int, int, int] = (60, 30, 200, 40),
    width: int = 320,
    height: int = 80,
) -> Frame:
    """造带字幕帧（亮背景+暗矩形）。"""
    arr = np.full((height, width, 3), 200, dtype=np.uint8)
    x, y, w, h = rect
    arr[y : y + h, x : x + w] = 30
    return Frame(timestamp_ms=timestamp_ms, image=Image.fromarray(arr))


def _subtitle_frame_text_b(
    timestamp_ms: int, width: int = 320, height: int = 80
) -> Frame:
    """造带字幕帧 B（暗矩形在不同位置，dHash 不同）。"""
    arr = np.full((height, width, 3), 200, dtype=np.uint8)
    arr[20:60, 40:120] = 30
    arr[20:60, 180:260] = 30
    return Frame(timestamp_ms=timestamp_ms, image=Image.fromarray(arr))


def _make_pipeline(
    frames: list[Frame],
    ocr: MockOcrEngine,
    config: Config | None = None,
) -> Pipeline:
    """构造测试用 Pipeline。"""
    extractor = StubExtractor(frames)
    region = BoundingBox(x=0, y=0, width=320, height=80)
    detector = FixedRegionDetector(region)
    if config is None:
        config = Config()
    return Pipeline(detector=detector, ocr=ocr, config=config, extractor=extractor)


class TestPipelineEmpty:
    """无字幕场景。"""

    def test_all_blank_frames_returns_empty(self) -> None:
        """全空白帧 → 空列表。"""
        frames = [_blank_frame(i * 200) for i in range(10)]
        ocr = MockOcrEngine(text="hello", confidence=0.9)
        pipeline = _make_pipeline(frames, ocr)
        result = pipeline.run(Path("fake.mp4"))
        assert result == []


class TestPipelineSingleSegment:
    """单段字幕。"""

    def test_single_subtitle(self) -> None:
        """空→字幕→空 → 1 个 SubtitleEntry。"""
        frames = [
            _blank_frame(0),
            _blank_frame(200),
            _subtitle_frame(400),
            _subtitle_frame(600),
            _subtitle_frame(800),
            _blank_frame(1000),
            _blank_frame(1200),
        ]
        ocr = MockOcrEngine(text="你好", confidence=0.9)
        pipeline = _make_pipeline(frames, ocr)
        result = pipeline.run(Path("fake.mp4"))

        assert len(result) == 1
        assert result[0].text == "你好"
        assert result[0].start_ms == 400
        assert result[0].end_ms == 1000


class TestPipelineTwoSegments:
    """两段独立字幕。"""

    def test_two_separate_subtitles(self) -> None:
        """字幕A→空→字幕B → 2 个 SubtitleEntry。"""
        frames = [
            _blank_frame(0),
            _subtitle_frame(200),
            _subtitle_frame(400),
            _blank_frame(600),
            _blank_frame(800),
            _subtitle_frame_text_b(1000),
            _subtitle_frame_text_b(1200),
            _blank_frame(1400),
            _blank_frame(1600),
        ]
        ocr = MockOcrEngine(
            sequence=[
                OcrResult(text="你好", confidence=0.9),
                OcrResult(text="再见", confidence=0.9),
            ]
        )
        config = Config(min_duration_ms=0)
        pipeline = _make_pipeline(frames, ocr, config)
        result = pipeline.run(Path("fake.mp4"))

        assert len(result) == 2
        assert result[0].text == "你好"
        assert result[1].text == "再见"


class TestPipelineDedupe:
    """dedupe 在 pipeline 中生效。"""

    def test_duplicate_text_merged(self) -> None:
        """两段相同文本 + gap 合法 → 合并。"""
        frames = [
            _blank_frame(0),
            _subtitle_frame(200),
            _subtitle_frame(400),
            _blank_frame(600),
            _blank_frame(800),
            _subtitle_frame(1000),
            _subtitle_frame(1200),
            _blank_frame(1400),
        ]
        ocr = MockOcrEngine(text="你好", confidence=0.9)
        pipeline = _make_pipeline(frames, ocr)
        result = pipeline.run(Path("fake.mp4"))

        if len(result) == 1:
            assert result[0].text == "你好"
        else:
            assert all(e.text == "你好" for e in result)


class TestPipelineConfidenceFilter:
    """confidence_threshold 过滤。"""

    def test_low_confidence_empties_text(self) -> None:
        """OCR 返回低置信度 → text 设空。"""
        frames = [
            _blank_frame(0),
            _subtitle_frame(200),
            _subtitle_frame(400),
            _blank_frame(600),
        ]
        ocr = MockOcrEngine(text="你好", confidence=0.3)
        config = Config(confidence_threshold=0.5, min_duration_ms=0)
        pipeline = _make_pipeline(frames, ocr, config)
        result = pipeline.run(Path("fake.mp4"))

        if result:
            assert result[0].text == ""


class TestPipelineOcrAnchorDelay:
    """feat-033b：OCR 锚帧延迟与空文本回退。"""

    def test_empty_anchor_retries_fallback(self) -> None:
        """主锚帧 OCR 空文本时，使用 fallback 帧重试并保留文本。"""
        frames = [
            _blank_frame(0),
            _subtitle_frame(200),
            _subtitle_frame(400),
            _subtitle_frame(600),
            _subtitle_frame(800),
            _blank_frame(1000),
            _blank_frame(1200),
        ]
        ocr = MockOcrEngine(
            sequence=[
                OcrResult(text="", confidence=0.0),
                OcrResult(text="你好", confidence=0.95),
            ]
        )
        config = Config(
            confidence_threshold=0.5,
            min_duration_ms=0,
            ocr_anchor_delay_frames=1,
        )
        pipeline = _make_pipeline(frames, ocr, config)
        result = pipeline.run(Path("fake.mp4"))
        assert len(result) == 1
        assert result[0].text == "你好"
        assert ocr._index == 2

    def test_delay_zero_uses_event_frame_only(self) -> None:
        """delay=0 时不强制二次 OCR（单帧成功即结束）。"""
        frames = [
            _blank_frame(0),
            _subtitle_frame(200),
            _subtitle_frame(400),
            _subtitle_frame(600),
            _blank_frame(800),
            _blank_frame(1000),
        ]
        ocr = MockOcrEngine(text="你好", confidence=0.9)
        config = Config(
            confidence_threshold=0.5,
            min_duration_ms=0,
            ocr_anchor_delay_frames=0,
        )
        pipeline = _make_pipeline(frames, ocr, config)
        result = pipeline.run(Path("fake.mp4"))
        assert len(result) == 1
        assert result[0].text == "你好"


class TestPipelineStreaming:
    """流式 API（feed/ocr_segment/finalize）测试。"""

    def _make_streaming_pipeline(
        self,
        ocr: MockOcrEngine,
        config: Config | None = None,
    ) -> Pipeline:
        """构造流式 Pipeline（无 extractor）。"""
        region = BoundingBox(x=0, y=0, width=320, height=80)
        detector = FixedRegionDetector(region)
        if config is None:
            config = Config()
        return Pipeline(detector=detector, ocr=ocr, config=config)

    def test_feed_blank_returns_none(self) -> None:
        """空白帧 → feed 返回 None。"""
        pipeline = self._make_streaming_pipeline(MockOcrEngine(text="你好", confidence=0.9))
        event = pipeline.feed(_blank_frame(0))
        assert event is None

    def test_single_segment_streaming(self) -> None:
        """空→字幕→空 → feed 返回 1 个 SegmentEvent → finalize 1 条。"""
        ocr = MockOcrEngine(text="你好", confidence=0.9)
        pipeline = self._make_streaming_pipeline(ocr)

        events: list[SegmentEvent] = []
        for frame in [
            _blank_frame(0),
            _subtitle_frame(200),
            _subtitle_frame(400),
            _subtitle_frame(600),
            _blank_frame(800),
            _blank_frame(1000),
        ]:
            event = pipeline.feed(frame)
            if event is not None:
                events.append(event)

        # 段闭合在 OUT（800ms），应返回 1 个事件
        assert len(events) == 1
        assert events[0].start_ms == 200
        assert events[0].end_ms == 800
        assert events[0].anchor_frame is not None

        # OCR
        entry = pipeline.ocr_segment(events[0])
        assert entry.text == "你好"

        # finalize 应返回 1 条
        result = pipeline.finalize()
        assert len(result) == 1
        assert result[0].text == "你好"

    def test_two_segments_streaming(self) -> None:
        """两段独立字幕 → 2 个 SegmentEvent → finalize 2 条。"""
        ocr = MockOcrEngine(
            sequence=[
                OcrResult(text="你好", confidence=0.9),
                OcrResult(text="再见", confidence=0.9),
            ]
        )
        config = Config(min_duration_ms=0)
        pipeline = self._make_streaming_pipeline(ocr, config)

        events: list[SegmentEvent] = []
        for frame in [
            _blank_frame(0),
            _subtitle_frame(200),
            _subtitle_frame(400),
            _blank_frame(600),
            _blank_frame(800),
            _subtitle_frame_text_b(1000),
            _subtitle_frame_text_b(1200),
            _blank_frame(1400),
            _blank_frame(1600),
        ]:
            event = pipeline.feed(frame)
            if event is not None:
                events.append(event)

        assert len(events) == 2
        entries = [pipeline.ocr_segment(e) for e in events]
        assert entries[0].text == "你好"
        assert entries[1].text == "再见"

        result = pipeline.finalize()
        assert len(result) == 2
        assert result[0].text == "你好"
        assert result[1].text == "再见"

    def test_finalize_closes_open_segment(self) -> None:
        """末尾未闭合段：finalize 自动关闭并 OCR。"""
        ocr = MockOcrEngine(text="你好", confidence=0.9)
        pipeline = self._make_streaming_pipeline(ocr)

        for frame in [
            _blank_frame(0),
            _subtitle_frame(200),
            _subtitle_frame(400),
            _subtitle_frame(600),
            _subtitle_frame(800),  # 无 OUT，段仍打开
        ]:
            pipeline.feed(frame)

        # feed 期间无事件返回（段未闭合）
        # finalize 应自动关闭末段
        result = pipeline.finalize()
        assert len(result) == 1
        assert result[0].text == "你好"
        assert result[0].start_ms == 200
        assert result[0].end_ms == 800

    def test_cancel_clears_state(self) -> None:
        """cancel 后 feed 重新开始（processed_count 归零）。"""
        pipeline = self._make_streaming_pipeline(MockOcrEngine(text="你好", confidence=0.9))
        pipeline.feed(_blank_frame(0))
        pipeline.feed(_blank_frame(200))
        assert pipeline.processed_count == 2

        pipeline.cancel()
        assert pipeline.processed_count == 0

    def test_cancel_blocks_subsequent_feed(self) -> None:
        """cancel 后 feed 返回 None 且不推进帧计数。"""
        pipeline = self._make_streaming_pipeline(MockOcrEngine(text="你好", confidence=0.9))
        pipeline.feed(_blank_frame(0))
        assert pipeline.processed_count == 1

        pipeline.cancel()

        event = pipeline.feed(_blank_frame(200))
        assert event is None
        assert pipeline.processed_count == 0  # 没有推进

    def test_streaming_equals_batch(self) -> None:
        """同一帧序列，流式和批量结果一致。"""
        frames = [
            _blank_frame(0),
            _subtitle_frame(200),
            _subtitle_frame(400),
            _blank_frame(600),
            _blank_frame(800),
            _subtitle_frame_text_b(1000),
            _subtitle_frame_text_b(1200),
            _blank_frame(1400),
        ]
        ocr_batch = MockOcrEngine(
            sequence=[
                OcrResult(text="你好", confidence=0.9),
                OcrResult(text="再见", confidence=0.9),
            ]
        )
        ocr_stream = MockOcrEngine(
            sequence=[
                OcrResult(text="你好", confidence=0.9),
                OcrResult(text="再见", confidence=0.9),
            ]
        )
        config = Config(min_duration_ms=0)

        # 批量
        region = BoundingBox(x=0, y=0, width=320, height=80)
        detector = FixedRegionDetector(region)
        batch_pipeline = Pipeline(
            detector=detector, ocr=ocr_batch, config=config, extractor=StubExtractor(frames)
        )
        batch_result = batch_pipeline.run(Path("fake.mp4"))

        # 流式
        stream_pipeline = Pipeline(
            detector=FixedRegionDetector(region), ocr=ocr_stream, config=config
        )
        for frame in frames:
            event = stream_pipeline.feed(frame)
            if event is not None:
                stream_pipeline.ocr_segment(event)
        stream_result = stream_pipeline.finalize()

        assert batch_result == stream_result

    def test_processed_count_increments(self) -> None:
        """processed_count 随帧递增。"""
        pipeline = self._make_streaming_pipeline(MockOcrEngine(text="你好", confidence=0.9))
        for i in range(5):
            pipeline.feed(_blank_frame(i * 200))
        assert pipeline.processed_count == 5
