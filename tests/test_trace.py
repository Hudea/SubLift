"""打轴决策 trace 记录测试。

用合成 FrameSignature 序列驱动状态机，验证各决策分支的 trace 记录完整性。
"""

from __future__ import annotations

import json
from pathlib import Path

from sublift.config import ChangePointConfig
from sublift.diagnostics.trace import TraceRecorder, load_jsonl
from sublift.pipeline.changepoint import ChangePointDetector
from sublift.pipeline.signature import FrameSignature


def _sig(
    timestamp_ms: int,
    foreground_ratio: float,
    dhash: int = 0,
) -> FrameSignature:
    return FrameSignature(
        timestamp_ms=timestamp_ms,
        foreground_ratio=foreground_ratio,
        dhash=dhash,
    )


def _feed(
    detector: ChangePointDetector,
    sigs: list[FrameSignature],
) -> None:
    for sig in sigs:
        detector.process(sig)


class TestTraceRecorder:
    """TraceRecorder 基础行为。"""

    def test_no_recorder_no_trace(self) -> None:
        """未注入 recorder 时，状态机正常工作且不报错。"""
        detector = ChangePointDetector(config=ChangePointConfig(hysteresis_frames=2))
        _feed(detector, [_sig(0, 0.0), _sig(200, 0.05), _sig(400, 0.05)])
        assert detector.current_state.name == "STABLE"

    def test_in_event_traced(self) -> None:
        """IN 事件记录 trigger=presence_rise, veto=none。

        trace 的 ``timestamp_ms`` 是确认帧（迟滞达标的那帧），事件本身
        的 ``timestamp_ms`` 回溯到首次出现帧（200）。两者不同是正确的。
        """
        recorder = TraceRecorder()
        detector = ChangePointDetector(
            config=ChangePointConfig(hysteresis_frames=2),
            trace_recorder=recorder,
        )
        events: list[object] = []
        for sig in [_sig(0, 0.0), _sig(200, 0.05), _sig(400, 0.05)]:
            ev = detector.process(sig)
            if ev is not None:
                events.append(ev)
        assert len(recorder.records) == 3

        in_records = [r for r in recorder.records if r.event_type == "IN"]
        assert len(in_records) == 1
        assert in_records[0].trigger_reason == "presence_rise"
        assert in_records[0].veto_reason == "none"
        assert in_records[0].state == "STABLE"
        # trace 记录确认帧时间戳；事件回溯时间戳在 event 对象上
        assert in_records[0].timestamp_ms == 400
        assert len(events) == 1
        assert events[0].timestamp_ms == 200  # type: ignore[attr-defined]

    def test_hysteresis_not_met_traced(self) -> None:
        """未达迟滞帧数记录 veto=hysteresis_not_met。"""
        recorder = TraceRecorder()
        detector = ChangePointDetector(
            config=ChangePointConfig(hysteresis_frames=3),
            trace_recorder=recorder,
        )
        _feed(detector, [_sig(0, 0.0), _sig(200, 0.05), _sig(400, 0.05)])
        assert len(recorder.records) == 3
        assert all(r.event_type is None for r in recorder.records)
        hysteresis_records = [r for r in recorder.records if r.veto_reason == "hysteresis_not_met"]
        assert len(hysteresis_records) == 2

    def test_out_event_traced(self) -> None:
        """OUT 事件记录 trigger=presence_fall。"""
        recorder = TraceRecorder()
        detector = ChangePointDetector(
            config=ChangePointConfig(hysteresis_frames=2),
            trace_recorder=recorder,
        )
        _feed(
            detector,
            [
                _sig(0, 0.05),
                _sig(200, 0.05),
                _sig(400, 0.0),
                _sig(600, 0.0),
            ],
        )
        out_records = [r for r in recorder.records if r.event_type == "OUT"]
        assert len(out_records) == 1
        assert out_records[0].trigger_reason == "presence_fall"
        assert out_records[0].state == "EMPTY"

    def test_change_event_traced(self) -> None:
        """CHANGE 事件记录 trigger=dhash_exceeds。"""
        recorder = TraceRecorder()
        detector = ChangePointDetector(
            config=ChangePointConfig(
                hysteresis_frames=2,
                change_threshold=5,
            ),
            trace_recorder=recorder,
        )
        _feed(
            detector,
            [
                _sig(0, 0.05, dhash=0b10101010),
                _sig(200, 0.05, dhash=0b10101010),
                _sig(400, 0.05, dhash=0b01010101),
                _sig(600, 0.05, dhash=0b01010101),
            ],
        )
        change_records = [r for r in recorder.records if r.event_type == "CHANGE"]
        assert len(change_records) == 1
        assert change_records[0].trigger_reason == "dhash_exceeds"
        assert change_records[0].veto_reason == "none"
        assert change_records[0].distance is not None
        assert change_records[0].distance > 5

    def test_distance_below_threshold_traced(self) -> None:
        """dHash 距离未超阈值记录 veto=distance_below_threshold。"""
        recorder = TraceRecorder()
        detector = ChangePointDetector(
            config=ChangePointConfig(
                hysteresis_frames=2,
                change_threshold=100,
            ),
            trace_recorder=recorder,
        )
        _feed(
            detector,
            [
                _sig(0, 0.05, dhash=0b10101010),
                _sig(200, 0.05, dhash=0b10101010),
                _sig(400, 0.05, dhash=0b01010101),
            ],
        )
        veto_records = [r for r in recorder.records if r.veto_reason == "distance_below_threshold"]
        assert len(veto_records) >= 1

    def test_unstable_content_traced(self) -> None:
        """dHash 超阈值但新内容未稳定记录 veto=unstable_content。"""
        recorder = TraceRecorder()
        detector = ChangePointDetector(
            config=ChangePointConfig(
                hysteresis_frames=2,
                change_threshold=5,
            ),
            trace_recorder=recorder,
        )
        _feed(
            detector,
            [
                _sig(0, 0.05, dhash=0b10101010),
                _sig(200, 0.05, dhash=0b10101010),
                _sig(400, 0.05, dhash=0b01010101),
                _sig(600, 0.05, dhash=0b11110000),
            ],
        )
        unstable_records = [r for r in recorder.records if r.veto_reason == "unstable_content"]
        assert len(unstable_records) >= 1

    def test_empty_state_no_subtitle_traced(self) -> None:
        """EMPTY 状态下无字幕记录 veto=none, trigger=none。"""
        recorder = TraceRecorder()
        detector = ChangePointDetector(
            config=ChangePointConfig(hysteresis_frames=2),
            trace_recorder=recorder,
        )
        _feed(detector, [_sig(0, 0.0), _sig(200, 0.0)])
        assert all(r.trigger_reason == "none" for r in recorder.records)
        assert all(r.veto_reason == "none" for r in recorder.records)


class TestTraceRecorderJsonl:
    """JSONL 文件 sink。"""

    def test_jsonl_roundtrip(self, tmp_path: Path) -> None:
        """写入 JSONL 后可加载回来，字段完整。"""
        jsonl_path = tmp_path / "trace.jsonl"
        recorder = TraceRecorder(jsonl_path=jsonl_path)
        detector = ChangePointDetector(
            config=ChangePointConfig(hysteresis_frames=2),
            trace_recorder=recorder,
        )
        _feed(
            detector,
            [
                _sig(0, 0.0),
                _sig(200, 0.05),
                _sig(400, 0.05),
                _sig(600, 0.0),
                _sig(800, 0.0),
            ],
        )
        recorder.flush()

        loaded = load_jsonl(jsonl_path)
        assert len(loaded) == len(recorder.records)
        assert loaded[0].timestamp_ms == 0
        assert loaded[2].event_type == "IN"
        assert loaded[2].trigger_reason == "presence_rise"
        assert loaded[4].event_type == "OUT"
        assert loaded[4].trigger_reason == "presence_fall"

    def test_jsonl_lines_valid(self, tmp_path: Path) -> None:
        """每行是合法 JSON 且含必需字段。"""
        jsonl_path = tmp_path / "trace.jsonl"
        recorder = TraceRecorder(jsonl_path=jsonl_path)
        detector = ChangePointDetector(
            config=ChangePointConfig(hysteresis_frames=2),
            trace_recorder=recorder,
        )
        _feed(detector, [_sig(0, 0.0), _sig(200, 0.05), _sig(400, 0.05)])
        recorder.flush()

        lines = jsonl_path.read_text(encoding="utf-8").strip().split("\n")
        assert len(lines) == 3
        for line in lines:
            data = json.loads(line)
            assert "timestamp_ms" in data
            assert "foreground_ratio" in data
            assert "has_subtitle" in data
            assert "dhash" in data
            assert "state" in data
            assert "trigger_reason" in data
            assert "veto_reason" in data

    def test_clear_keeps_file(self, tmp_path: Path) -> None:
        """clear() 清空内存但不动文件。"""
        jsonl_path = tmp_path / "trace.jsonl"
        recorder = TraceRecorder(jsonl_path=jsonl_path)
        detector = ChangePointDetector(
            config=ChangePointConfig(hysteresis_frames=2),
            trace_recorder=recorder,
        )
        _feed(detector, [_sig(0, 0.0), _sig(200, 0.05), _sig(400, 0.05)])
        file_lines = len(jsonl_path.read_text(encoding="utf-8").strip().split("\n"))
        recorder.clear()
        assert recorder.records == []
        assert len(jsonl_path.read_text(encoding="utf-8").strip().split("\n")) == file_lines


class TestTraceFields:
    """trace 记录字段完整性。"""

    def test_anchor_dhash_none_in_empty(self) -> None:
        """EMPTY 状态下 anchor_dhash 为 None。"""
        recorder = TraceRecorder()
        detector = ChangePointDetector(
            config=ChangePointConfig(hysteresis_frames=2),
            trace_recorder=recorder,
        )
        _feed(detector, [_sig(0, 0.0), _sig(200, 0.0)])
        assert all(r.anchor_dhash is None for r in recorder.records)

    def test_anchor_dhash_set_in_stable(self) -> None:
        """STABLE 状态下 anchor_dhash 有值。"""
        recorder = TraceRecorder()
        detector = ChangePointDetector(
            config=ChangePointConfig(hysteresis_frames=2),
            trace_recorder=recorder,
        )
        _feed(
            detector,
            [
                _sig(0, 0.05, dhash=0b10101010),
                _sig(200, 0.05, dhash=0b10101010),
                _sig(400, 0.05, dhash=0b10101010),
            ],
        )
        stable_records = [r for r in recorder.records if r.state == "STABLE"]
        assert len(stable_records) >= 1
        assert all(r.anchor_dhash == 0b10101010 for r in stable_records)

    def test_has_subtitle_field(self) -> None:
        """has_subtitle 字段正确反映前景占比判断。"""
        recorder = TraceRecorder()
        detector = ChangePointDetector(
            config=ChangePointConfig(hysteresis_frames=1, presence_threshold=0.02),
            trace_recorder=recorder,
        )
        _feed(detector, [_sig(0, 0.0), _sig(200, 0.05)])
        assert recorder.records[0].has_subtitle is False
        assert recorder.records[1].has_subtitle is True
