"""时间轴构建测试。"""

from __future__ import annotations

from sublift.pipeline.changepoint import EventType, StateEvent
from sublift.pipeline.timeline import TimelineBuilder, TimelineSegment


def _event(
    event_type: EventType,
    timestamp_ms: int,
    prev_end_ms: int | None = None,
) -> StateEvent:
    return StateEvent(
        event_type=event_type,
        timestamp_ms=timestamp_ms,
        prev_end_ms=prev_end_ms,
    )


class TestTimelineBuilder:
    """TimelineBuilder 行为。"""

    def test_single_segment(self) -> None:
        """IN + OUT → 单段。"""
        builder = TimelineBuilder()
        builder.consume(_event(EventType.IN, 0))
        builder.consume(_event(EventType.OUT, 1000))
        segments = builder.build()
        assert segments == [TimelineSegment(start_ms=0, end_ms=1000)]

    def test_change_splits_segment(self) -> None:
        """IN + CHANGE + OUT → 两段。"""
        builder = TimelineBuilder()
        builder.consume(_event(EventType.IN, 0))
        builder.consume(_event(EventType.CHANGE, 500, prev_end_ms=500))
        builder.consume(_event(EventType.OUT, 1000))
        segments = builder.build()
        assert segments == [
            TimelineSegment(start_ms=0, end_ms=500),
            TimelineSegment(start_ms=500, end_ms=1000),
        ]

    def test_open_segment_finalized(self) -> None:
        """末尾未关闭段用 finalize_open_segment 关闭。"""
        builder = TimelineBuilder()
        builder.consume(_event(EventType.IN, 0))
        builder.finalize_open_segment(2000)
        segments = builder.build()
        assert segments == [TimelineSegment(start_ms=0, end_ms=2000)]

    def test_multiple_segments(self) -> None:
        """多个独立段。"""
        builder = TimelineBuilder()
        builder.consume(_event(EventType.IN, 0))
        builder.consume(_event(EventType.OUT, 1000))
        builder.consume(_event(EventType.IN, 2000))
        builder.consume(_event(EventType.OUT, 3000))
        segments = builder.build()
        assert len(segments) == 2
        assert segments[0] == TimelineSegment(start_ms=0, end_ms=1000)
        assert segments[1] == TimelineSegment(start_ms=2000, end_ms=3000)

    def test_reset(self) -> None:
        builder = TimelineBuilder()
        builder.consume(_event(EventType.IN, 0))
        builder.reset()
        assert builder.build() == []

    def test_duration_ms(self) -> None:
        seg = TimelineSegment(start_ms=100, end_ms=600)
        assert seg.duration_ms == 500

    def test_duration_ms_open_segment(self) -> None:
        seg = TimelineSegment(start_ms=100, end_ms=None)
        assert seg.duration_ms == 0
