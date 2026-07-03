"""字幕时间轴构建。

消费变化点状态机的事件流，构建带起止时间的字幕段。

配合 ChangePointDetector 使用：
    detector = ChangePointDetector()
    builder = TimelineBuilder()

    for signature, crop in stream:
        event = detector.process(signature, crop)
        if event:
            builder.consume(event)

    segments = builder.build()
"""

from __future__ import annotations

from dataclasses import dataclass, field

from sublift.pipeline.changepoint import EventType, StateEvent


@dataclass
class TimelineSegment:
    """单个字幕段时间轴段。

    Attributes:
        start_ms: 字幕出现时间戳（毫秒）。
        end_ms: 字幕消失时间戳（毫秒）；None 表示视频结束时字幕仍在。
    """

    start_ms: int
    end_ms: int | None = None

    @property
    def duration_ms(self) -> int:
        """持续时间（毫秒）。end_ms 为 None 时返回 0。"""
        if self.end_ms is None:
            return 0
        return self.end_ms - self.start_ms


@dataclass
class TimelineBuilder:
    """时间轴构建器（有状态）。

    消费 StateEvent 流，产出 TimelineSegment 列表。
    """

    _segments: list[TimelineSegment] = field(default_factory=list, init=False)
    _current_start_ms: int | None = field(default=None, init=False)

    def consume(self, event: StateEvent) -> None:
        """消费一个状态事件，更新时间轴。

        Args:
            event: 状态机产出的事件。
        """
        if event.event_type == EventType.IN:
            self._start_segment(event.timestamp_ms)
        elif event.event_type == EventType.OUT:
            self._end_segment(event.timestamp_ms)
        elif event.event_type == EventType.CHANGE:
            assert event.prev_end_ms is not None
            self._end_segment(event.prev_end_ms)
            self._start_segment(event.timestamp_ms)

    def finalize_open_segment(self, last_timestamp_ms: int) -> None:
        """关闭末尾未关闭的段（视频结束时字幕仍在）。

        Args:
            last_timestamp_ms: 最后一帧的时间戳。
        """
        if self._current_start_ms is not None:
            self._end_segment(last_timestamp_ms)

    def build(self) -> list[TimelineSegment]:
        """构建并返回最终的时间轴段列表。"""
        return list(self._segments)

    def reset(self) -> None:
        """重置构建器状态。"""
        self._segments = []
        self._current_start_ms = None

    def _start_segment(self, start_ms: int) -> None:
        self._current_start_ms = start_ms
        self._segments.append(TimelineSegment(start_ms=start_ms))

    def _end_segment(self, end_ms: int) -> None:
        if self._current_start_ms is None:
            return
        self._segments[-1].end_ms = end_ms
        self._current_start_ms = None
