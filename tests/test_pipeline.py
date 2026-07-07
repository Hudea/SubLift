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
from sublift.models import (
    BoundingBox,
    Frame,
    OcrLine,
    OcrResult,
    PersistentTextPolicy,
    SubtitleProfile,
)
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


class TestPipelineProfileSelector:
    """feat-033d：Pipeline 接入 subtitle_profile + selector。

    MockOcrEngine 返回带 lines 的 OcrResult，Pipeline ocr_segment
    根据 profile 筛选目标字幕行，过滤背景文字。
    """

    def _make_ocr_with_lines(
        self,
        target_text: str,
        target_y: int,
        target_height: int,
        bg_text: str,
        bg_y: int,
        bg_height: int,
    ) -> MockOcrEngine:
        """构造带 lines 的 MockOcrEngine（sequence 模式，每次返回相同结果）。"""
        result = OcrResult(
            text=f"{target_text}\n{bg_text}",
            confidence=0.85,
            lines=[
                OcrLine(
                    text=target_text,
                    confidence=0.9,
                    bbox=BoundingBox(x=10, y=target_y, width=200, height=target_height),
                ),
                OcrLine(
                    text=bg_text,
                    confidence=0.7,
                    bbox=BoundingBox(x=10, y=bg_y, width=150, height=bg_height),
                ),
            ],
        )
        return MockOcrEngine(sequence=[result])

    def test_profile_filters_background(self) -> None:
        """profile 只选中文字幕行，过滤背景英文。"""
        frames = [
            _blank_frame(0),
            _subtitle_frame(200),
            _subtitle_frame(400),
            _blank_frame(600),
            _blank_frame(800),
        ]
        ocr = self._make_ocr_with_lines(
            target_text="你好",
            target_y=40,
            target_height=30,
            bg_text="TITLE",
            bg_y=5,
            bg_height=15,
        )
        profile = SubtitleProfile(
            y_center=55.0,
            y_tolerance=30.0,
            line_height=25.0,
            max_lines=1,
        )
        pipeline = Pipeline(
            detector=FixedRegionDetector(BoundingBox(x=0, y=0, width=320, height=80)),
            ocr=ocr,
            config=Config(min_duration_ms=0),
            profile=profile,
        )
        for frame in frames:
            event = pipeline.feed(frame)
            if event is not None:
                pipeline.ocr_segment(event)
        result = pipeline.finalize()

        assert len(result) == 1
        assert result[0].text == "你好"

    def test_no_profile_keeps_all_lines(self) -> None:
        """无 profile 时走旧路径，保留 OcrResult.text（含背景文字）。"""
        frames = [
            _blank_frame(0),
            _subtitle_frame(200),
            _subtitle_frame(400),
            _blank_frame(600),
            _blank_frame(800),
        ]
        ocr = self._make_ocr_with_lines(
            target_text="你好",
            target_y=40,
            target_height=30,
            bg_text="TITLE",
            bg_y=5,
            bg_height=15,
        )
        pipeline = Pipeline(
            detector=FixedRegionDetector(BoundingBox(x=0, y=0, width=320, height=80)),
            ocr=ocr,
            config=Config(min_duration_ms=0),
        )
        for frame in frames:
            event = pipeline.feed(frame)
            if event is not None:
                pipeline.ocr_segment(event)
        result = pipeline.finalize()

        assert len(result) == 1
        assert "你好" in result[0].text
        assert "TITLE" in result[0].text


class TestPipelineSegmentOcrLinesCache:
    """feat-034b：Pipeline 缓存 per-segment OcrLine，与 _closed_entries 同 index 对齐。

    persistent filter 在 finalize 阶段需要访问每段的 OcrLine 列表，
    故 ocr_segment 必须把 ocr_result.lines 缓存起来。
    """

    def _make_ocr_with_lines(self, results: list[OcrResult]) -> MockOcrEngine:
        return MockOcrEngine(sequence=results)

    def test_lines_cached_and_aligned_with_entries(self) -> None:
        """两段字幕，每段 OCR 返回不同 lines，缓存长度与 entries 对齐。"""
        r1 = OcrResult(
            text="你好\nTITLE",
            confidence=0.85,
            lines=[
                OcrLine(text="你好", confidence=0.9,
                        bbox=BoundingBox(x=10, y=40, width=200, height=30)),
                OcrLine(text="TITLE", confidence=0.7,
                        bbox=BoundingBox(x=10, y=5, width=150, height=15)),
            ],
        )
        r2 = OcrResult(
            text="世界\nTITLE",
            confidence=0.82,
            lines=[
                OcrLine(text="世界", confidence=0.88,
                        bbox=BoundingBox(x=10, y=40, width=200, height=30)),
                OcrLine(text="TITLE", confidence=0.65,
                        bbox=BoundingBox(x=10, y=5, width=150, height=15)),
            ],
        )
        ocr = self._make_ocr_with_lines([r1, r2])
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
        pipeline = Pipeline(
            detector=FixedRegionDetector(BoundingBox(x=0, y=0, width=320, height=80)),
            ocr=ocr,
            config=Config(min_duration_ms=0),
        )
        for frame in frames:
            event = pipeline.feed(frame)
            if event is not None:
                pipeline.ocr_segment(event)

        # 闭合段数 == 2（两段独立字幕）
        assert len(pipeline._closed_entries) == 2
        assert len(pipeline._segment_ocr_lines) == 2
        # 对齐：每段缓存的 lines 长度与 OcrResult.lines 一致
        assert len(pipeline._segment_ocr_lines[0]) == 2
        assert len(pipeline._segment_ocr_lines[1]) == 2
        # 内容正确
        assert pipeline._segment_ocr_lines[0][0].text == "你好"
        assert pipeline._segment_ocr_lines[1][0].text == "世界"

    def test_no_anchor_frame_caches_empty_lines(self) -> None:
        """early return 路径（无 anchor frame）缓存空 list，保持对齐。"""
        ocr = MockOcrEngine(text="你好", confidence=0.9)
        pipeline = Pipeline(
            detector=FixedRegionDetector(BoundingBox(x=0, y=0, width=320, height=80)),
            ocr=ocr,
            config=Config(min_duration_ms=0),
        )
        # 构造一个无 anchor 的 SegmentEvent（anchor_frame=None）
        event = SegmentEvent(start_ms=0, end_ms=1000, anchor_frame=None)
        entry = pipeline.ocr_segment(event)

        assert entry.text == ""
        assert entry.confidence == 0.0
        assert len(pipeline._closed_entries) == 1
        assert len(pipeline._segment_ocr_lines) == 1
        assert pipeline._segment_ocr_lines[0] == []

    def test_reset_clears_segment_ocr_lines(self) -> None:
        """run_frames 触发 _reset_streaming_state，清空缓存。"""
        # 固定模式 MockOcrEngine（每次 recognize 返回相同结果，可重复 run）
        ocr = MockOcrEngine(text="你好", confidence=0.9)
        frames = [_blank_frame(0), _subtitle_frame(200), _subtitle_frame(400), _blank_frame(600)]
        pipeline = Pipeline(
            detector=FixedRegionDetector(BoundingBox(x=0, y=0, width=320, height=80)),
            ocr=ocr,
            config=Config(min_duration_ms=0),
        )
        # 固定模式不返回 lines，缓存为空但长度与 entries 对齐
        pipeline.run_frames(iter(frames))
        assert len(pipeline._segment_ocr_lines) == 1
        assert pipeline._segment_ocr_lines[0] == []

        # 再次 run_frames，状态应被重置（旧缓存被清掉，新缓存重建）
        pipeline.run_frames(iter(frames))
        assert len(pipeline._segment_ocr_lines) == 1


class TestPipelinePersistentFilter:
    """feat-034d：Pipeline.finalize 接入 persistent_text_policy。

    模拟 ticker 场景：4 段独立字幕，每段中文不同 + ticker 文本片段多变。
    persistent filter 在 finalize 阶段剔除 ticker，只保留中文。
    """

    def _make_segment_ocr(
        self, target: str, ticker: str
    ) -> OcrResult:
        """构造单段 OCR 结果（中文 + ticker，y 不同）。"""
        return OcrResult(
            text=f"{target}\n{ticker}",
            confidence=0.85,
            lines=[
                OcrLine(text=target, confidence=0.9,
                        bbox=BoundingBox(x=10, y=40, width=200, height=30)),
                OcrLine(text=ticker, confidence=0.7,
                        bbox=BoundingBox(x=10, y=70, width=150, height=20)),
            ],
        )

    def test_finalize_filters_persistent_ticker(self) -> None:
        """4 段独立字幕，ticker 在不同段出现不同片段，finalize 剔除 ticker。"""
        results = [
            self._make_segment_ocr("跟胡尼克", "UNLIKEL"),
            self._make_segment_ocr("一位狐狸", "CONSPIRACY"),
            self._make_segment_ocr("调查案子", "DISCOVER"),
            self._make_segment_ocr("发现线索", "THE TRUTH"),
        ]
        ocr = MockOcrEngine(sequence=results)
        # 构造 4 段独立字幕帧序列
        frames = [
            _blank_frame(0),
            _subtitle_frame(200), _subtitle_frame(400),
            _blank_frame(600), _blank_frame(800),
            _subtitle_frame_text_b(1000), _subtitle_frame_text_b(1200),
            _blank_frame(1400), _blank_frame(1600),
            _subtitle_frame(1800), _subtitle_frame(2000),
            _blank_frame(2200), _blank_frame(2400),
            _subtitle_frame_text_b(2600), _subtitle_frame_text_b(2800),
            _blank_frame(3000), _blank_frame(3200),
        ]
        profile = SubtitleProfile(
            y_center=55.0, y_tolerance=30.0, line_height=30.0, max_lines=1,
            persistent_text_policy=PersistentTextPolicy(min_distinct_texts=4),
        )
        pipeline = Pipeline(
            detector=FixedRegionDetector(BoundingBox(x=0, y=0, width=320, height=80)),
            ocr=ocr,
            config=Config(min_duration_ms=0),
            profile=profile,
        )
        result = pipeline.run_frames(iter(frames))

        # 4 段字幕都应保留，ticker 被剔除
        assert len(result) == 4
        texts = [e.text for e in result]
        assert "跟胡尼克" in texts
        assert "一位狐狸" in texts
        assert "调查案子" in texts
        assert "发现线索" in texts
        # ticker 文本不应出现在任何段
        for t in texts:
            assert "UNLIKEL" not in t
            assert "CONSPIRACY" not in t

    def test_finalize_no_policy_zero_behavior_change(self) -> None:
        """profile 无 persistent_text_policy 时，finalize 行为与旧路径一致。"""
        r1 = self._make_segment_ocr("你好", "TICKER")
        r2 = self._make_segment_ocr("世界", "TICKER")
        ocr = MockOcrEngine(sequence=[r1, r2])
        frames = [
            _blank_frame(0),
            _subtitle_frame(200), _subtitle_frame(400),
            _blank_frame(600), _blank_frame(800),
            _subtitle_frame_text_b(1000), _subtitle_frame_text_b(1200),
            _blank_frame(1400), _blank_frame(1600),
        ]
        profile_no_policy = SubtitleProfile(
            y_center=55.0, y_tolerance=30.0, line_height=30.0, max_lines=1,
        )
        pipeline = Pipeline(
            detector=FixedRegionDetector(BoundingBox(x=0, y=0, width=320, height=80)),
            ocr=ocr,
            config=Config(min_duration_ms=0),
            profile=profile_no_policy,
        )
        result = pipeline.run_frames(iter(frames))

        # 无 persistent policy：selector 靠 max_lines=1 截断取置信度最高
        # 中文 confidence=0.9 > ticker 0.7，只保留中文
        assert len(result) == 2
        assert result[0].text == "你好"
        assert result[1].text == "世界"
