"""变化点状态机测试。

用合成 FrameSignature 序列驱动状态机，不依赖图像处理。
"""

from __future__ import annotations

import numpy as np

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


def _text_image(rect_x: int = 60) -> np.ndarray:
    """造带暗色字幕矩形的亮背景图（模拟字幕带）。

    rect_x 控制字幕水平位置，用于构造 SSIM 差异。
    """
    img = np.full((80, 320, 3), 200, dtype=np.uint8)
    img[30:70, rect_x : rect_x + 200] = 30
    return img


class TestSsimPatrol:
    """SSIM 巡逻测试（feat-031b）。

    patrol 在 dHash 未触发时主动比较前景结构，检测连续字幕切换。
    需要传真实 crop 图像（numpy 合成），因为 SSIM 在二值图上计算。
    """

    def test_patrol_disabled_no_change(self) -> None:
        """patrol 关闭时，dHash 未触发 → 无 CHANGE。"""
        detector = ChangePointDetector(
            config=ChangePointConfig(
                hysteresis_frames=2,
                change_threshold=100,
                enable_ssim_patrol=False,
            )
        )
        img_a = _text_image(60)
        img_b = _text_image(200)
        detector.process(_sig(0, 0.05, dhash=0b10101010), img_a)
        detector.process(_sig(200, 0.05, dhash=0b10101010), img_a)
        event = detector.process(_sig(400, 0.05, dhash=0b10101010), img_b)
        assert event is None

    def test_patrol_triggers_change_on_structure_diff(self) -> None:
        """patrol 启用时，dHash 未触发但 SSIM 显示结构变化 → CHANGE。"""
        detector = ChangePointDetector(
            config=ChangePointConfig(
                hysteresis_frames=2,
                change_threshold=100,
                enable_ssim_patrol=True,
                ssim_patrol_interval=1,
                ssim_patrol_threshold=0.95,
            )
        )
        img_a = _text_image(60)
        img_b = _text_image(200)

        detector.process(_sig(0, 0.05, dhash=0b10101010), img_a)
        detector.process(_sig(200, 0.05, dhash=0b10101010), img_a)

        events: list[StateEvent] = []
        for ts in range(400, 1200, 200):
            ev = detector.process(_sig(ts, 0.05, dhash=0b10101010), img_b)
            if ev is not None:
                events.append(ev)

        change_events = [e for e in events if e.event_type == EventType.CHANGE]
        assert len(change_events) >= 1

    def test_patrol_interval_respected(self) -> None:
        """patrol 间隔生效：间隔=3 时前 2 帧不巡逻。"""
        detector = ChangePointDetector(
            config=ChangePointConfig(
                hysteresis_frames=2,
                change_threshold=100,
                enable_ssim_patrol=True,
                ssim_patrol_interval=3,
                ssim_patrol_threshold=0.95,
            )
        )
        img_a = _text_image(60)
        img_b = _text_image(200)

        detector.process(_sig(0, 0.05, dhash=0b10101010), img_a)
        detector.process(_sig(200, 0.05, dhash=0b10101010), img_a)

        event = detector.process(_sig(400, 0.05, dhash=0b10101010), img_b)
        assert event is None

    def test_patrol_no_change_on_identical_structure(self) -> None:
        """patrol 启用但结构相同时 → 无 CHANGE。"""
        detector = ChangePointDetector(
            config=ChangePointConfig(
                hysteresis_frames=2,
                change_threshold=100,
                enable_ssim_patrol=True,
                ssim_patrol_interval=1,
                ssim_patrol_threshold=0.95,
            )
        )
        img = _text_image(60)
        detector.process(_sig(0, 0.05, dhash=0b10101010), img)
        detector.process(_sig(200, 0.05, dhash=0b10101010), img)
        event = detector.process(_sig(400, 0.05, dhash=0b10101010), img)
        assert event is None

    def test_patrol_candidate_requires_stable_confirmation(self) -> None:
        """patrol 产生候选后需稳定确认才触发 CHANGE。

        构造：第 3 帧 SSIM 变化产生候选，第 4 帧仍变化（未稳定），
        第 5 帧恢复稳定结构 → 确认 CHANGE。
        """
        detector = ChangePointDetector(
            config=ChangePointConfig(
                hysteresis_frames=2,
                change_threshold=100,
                enable_ssim_patrol=True,
                ssim_patrol_interval=1,
                ssim_patrol_threshold=0.95,
            )
        )
        img_a = _text_image(60)
        img_b = _text_image(200)

        detector.process(_sig(0, 0.05, dhash=0b10101010), img_a)
        detector.process(_sig(200, 0.05, dhash=0b10101010), img_a)

        ev = detector.process(_sig(400, 0.05, dhash=0b10101010), img_b)
        assert ev is None

        ev = detector.process(_sig(600, 0.05, dhash=0b10101010), img_b)
        change_events = []
        if ev is not None and ev.event_type == EventType.CHANGE:
            change_events.append(ev)

        assert len(change_events) == 1

    def test_patrol_coexists_with_dhash(self) -> None:
        """patrol 与 dHash 候选可共存（dHash 触发优先）。

        dHash 超阈值时走 dHash 路径，不走 patrol。需要 2 帧稳定确认。
        """
        detector = ChangePointDetector(
            config=ChangePointConfig(
                hysteresis_frames=2,
                change_threshold=5,
                enable_ssim_patrol=True,
                ssim_patrol_interval=1,
                ssim_patrol_threshold=0.95,
            )
        )
        img_a = _text_image(60)
        img_b = _text_image(200)

        detector.process(_sig(0, 0.05, dhash=0b10101010), img_a)
        detector.process(_sig(200, 0.05, dhash=0b10101010), img_a)

        events: list[StateEvent] = []
        for ts in (400, 600):
            ev = detector.process(_sig(ts, 0.05, dhash=0b01010101), img_b)
            if ev is not None:
                events.append(ev)

        change_events = [e for e in events if e.event_type == EventType.CHANGE]
        assert len(change_events) == 1
