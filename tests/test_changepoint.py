"""变化点状态机测试。

用合成 FrameSignature 序列驱动状态机，不依赖图像处理。
"""

from __future__ import annotations

from sublift.config import ChangePointConfig
from sublift.pipeline.changepoint import (
    ChangePointDetector,
    EventType,
    State,
    StateEvent,
)
from sublift.pipeline.signature import FrameSignature


def _sig(
    timestamp_ms: int,
    foreground_ratio: float,
    dhash: int = 0,
) -> FrameSignature:
    """造一个签名。"""
    return FrameSignature(
        timestamp_ms=timestamp_ms,
        foreground_ratio=foreground_ratio,
        dhash=dhash,
    )


def _feed(
    detector: ChangePointDetector,
    sigs: list[FrameSignature],
) -> list[StateEvent]:
    """喂入签名序列，收集所有事件。"""
    events: list[StateEvent] = []
    for sig in sigs:
        event = detector.process(sig)
        if event is not None:
            events.append(event)
    return events


class TestAppearance:
    """字幕出现：EMPTY → STABLE。"""

    def test_appearance_emits_in_event(self) -> None:
        """连续 N 帧有字幕 → IN 事件。"""
        detector = ChangePointDetector(
            config=ChangePointConfig(hysteresis_frames=2)
        )
        events = _feed(
            detector,
            [
                _sig(0, 0.0),
                _sig(200, 0.05),
                _sig(400, 0.05),
            ],
        )
        assert len(events) == 1
        assert events[0].event_type == EventType.IN
        assert events[0].timestamp_ms == 200
        assert detector.current_state == State.STABLE

    def test_appearance_timestamp_uses_first_frame(self) -> None:
        """IN 事件时间戳 = 首次检测帧，非迟滞确认帧。"""
        detector = ChangePointDetector(
            config=ChangePointConfig(hysteresis_frames=3)
        )
        events = _feed(
            detector,
            [
                _sig(0, 0.0),
                _sig(200, 0.05),
                _sig(400, 0.05),
                _sig(600, 0.05),
            ],
        )
        assert len(events) == 1
        assert events[0].timestamp_ms == 200

    def test_flicker_no_in_event(self) -> None:
        """单帧闪烁（未达迟滞）不触发 IN。"""
        detector = ChangePointDetector(
            config=ChangePointConfig(hysteresis_frames=2)
        )
        events = _feed(
            detector,
            [
                _sig(0, 0.0),
                _sig(200, 0.05),
                _sig(400, 0.0),
            ],
        )
        assert len(events) == 0
        assert detector.current_state == State.EMPTY


class TestDisappearance:
    """字幕消失：STABLE → EMPTY。"""

    def test_disappearance_emits_out_event(self) -> None:
        """连续 N 帧无字幕 → OUT 事件。"""
        detector = ChangePointDetector(
            config=ChangePointConfig(hysteresis_frames=2)
        )
        events = _feed(
            detector,
            [
                _sig(0, 0.05),
                _sig(200, 0.05),
                _sig(400, 0.0),
                _sig(600, 0.0),
            ],
        )
        assert len(events) == 2
        assert events[0].event_type == EventType.IN
        assert events[1].event_type == EventType.OUT
        assert events[1].timestamp_ms == 400

    def test_disappearance_flicker_no_out(self) -> None:
        """单帧无字幕闪烁不触发 OUT。"""
        detector = ChangePointDetector(
            config=ChangePointConfig(hysteresis_frames=2)
        )
        events = _feed(
            detector,
            [
                _sig(0, 0.05),
                _sig(200, 0.05),
                _sig(400, 0.0),
                _sig(600, 0.05),
            ],
        )
        in_count = sum(1 for e in events if e.event_type == EventType.IN)
        out_count = sum(1 for e in events if e.event_type == EventType.OUT)
        assert in_count == 1
        assert out_count == 0


class TestContentChange:
    """字幕内容变化：STABLE → STABLE_PRIME。"""

    def test_change_emits_change_event(self) -> None:
        """dHash 变化 + 新内容稳定 → CHANGE 事件。"""
        detector = ChangePointDetector(
            config=ChangePointConfig(
                hysteresis_frames=2,
                change_threshold=5,
            )
        )
        events = _feed(
            detector,
            [
                _sig(0, 0.05, dhash=0b10101010),
                _sig(200, 0.05, dhash=0b10101010),
                _sig(400, 0.05, dhash=0b01010101),
                _sig(600, 0.05, dhash=0b01010101),
            ],
        )
        change_events = [e for e in events if e.event_type == EventType.CHANGE]
        assert len(change_events) == 1
        assert change_events[0].timestamp_ms == 400
        assert change_events[0].prev_end_ms == 400

    def test_jitter_no_change_event(self) -> None:
        """单帧抖动后恢复 → 不触发 CHANGE。"""
        detector = ChangePointDetector(
            config=ChangePointConfig(
                hysteresis_frames=2,
                change_threshold=5,
            )
        )
        events = _feed(
            detector,
            [
                _sig(0, 0.05, dhash=0b10101010),
                _sig(200, 0.05, dhash=0b10101010),
                _sig(400, 0.05, dhash=0b01010101),
                _sig(600, 0.05, dhash=0b10101010),
            ],
        )
        change_events = [e for e in events if e.event_type == EventType.CHANGE]
        assert len(change_events) == 0


class TestPersistence:
    """字幕持续：无事件。"""

    def test_stable_no_event(self) -> None:
        """STABLE 状态下相同帧 → 无事件。"""
        detector = ChangePointDetector(
            config=ChangePointConfig(hysteresis_frames=2)
        )
        events = _feed(
            detector,
            [
                _sig(0, 0.05, dhash=0b10101010),
                _sig(200, 0.05, dhash=0b10101010),
                _sig(400, 0.05, dhash=0b10101010),
                _sig(600, 0.05, dhash=0b10101010),
            ],
        )
        in_count = sum(1 for e in events if e.event_type == EventType.IN)
        assert in_count == 1
        assert len(events) == 1


class TestReset:
    """reset() 行为。"""

    def test_reset_clears_state(self) -> None:
        detector = ChangePointDetector(
            config=ChangePointConfig(hysteresis_frames=2)
        )
        _feed(detector, [_sig(0, 0.05), _sig(200, 0.05)])
        assert detector.current_state == State.STABLE

        detector.reset()
        current: State = detector.current_state
        assert current == State.EMPTY
