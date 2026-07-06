"""变化点检测模块（状态机）。

基于帧签名的状态机，检测字幕的出现、消失和内容变化。

状态机：
  ┌───────┐  A=1    ┌───────┐  B变化   ┌────────┐
  │ EMPTY │◀───────▶│STABLE │◀────────▶│STABLE' │
  └───────┘  A=0    └───────┘           └────────┘

信号：
  - 信号 A：前景像素占比 → 检测「出现 / 消失」边界
  - 信号 B：dHash 汉明距离 → 检测「内容变化」边界

关键机制：
  - 迟滞确认：连续 N 帧满足条件才迁移状态，抗单帧闪烁
  - 时间戳回溯：使用信号首次出现的帧时间戳，而非迟滞确认帧，保证打轴精度
  - 候选/稳定确认双阶段：dHash 触发后等新内容稳定才确认变化，抗抖动
  - SSIM 两级验证（可选）：dHash 粗筛 + SSIM 细验，过滤误报
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum, auto
from typing import TYPE_CHECKING

import numpy as np

from sublift.config import ChangePointConfig
from sublift.pipeline.signature import (
    FrameSignature,
    compute_foreground_ssim,
    compute_ssim,
    hamming_distance,
)

if TYPE_CHECKING:
    from sublift.diagnostics.trace import TraceRecorder, TriggerReason, VetoReason


class State(Enum):
    """状态机状态。"""

    EMPTY = auto()
    """无字幕。"""

    STABLE = auto()
    """字幕稳定（有锚帧）。"""

    STABLE_PRIME = auto()
    """字幕稳定（内容变化后的新稳定态）。"""


class EventType(Enum):
    """状态变化事件类型。"""

    IN = auto()
    """字幕出现。"""

    OUT = auto()
    """字幕消失。"""

    CHANGE = auto()
    """字幕内容变化。"""


@dataclass
class StateEvent:
    """状态变化事件。

    Attributes:
        event_type: 事件类型。
        timestamp_ms: 事件发生的时间戳（信号首次出现的帧，非迟滞确认帧）。
        prev_end_ms: 前一个字幕段的结束时间（仅 CHANGE 事件）。
    """

    event_type: EventType
    timestamp_ms: int
    prev_end_ms: int | None = None


@dataclass
class ChangePointDetector:
    """变化点检测器（有状态）。

    使用方法：
        detector = ChangePointDetector()
        for signature, crop in stream:
            event = detector.process(signature, crop)
            if event:
                # 处理状态变化事件
                ...

    Attributes:
        config: 变化点检测配置。
    """

    config: ChangePointConfig = field(default_factory=ChangePointConfig)
    trace_recorder: TraceRecorder | None = field(default=None)

    _state: State = field(default=State.EMPTY, init=False)
    _anchor_signature: FrameSignature | None = field(default=None, init=False)
    _anchor_crop: np.ndarray | None = field(default=None, init=False)
    _last_signature: FrameSignature | None = field(default=None, init=False)
    _consecutive_presence: int = field(default=0, init=False)
    _consecutive_absence: int = field(default=0, init=False)
    _stable_start_ms: int | None = field(default=None, init=False)
    _first_presence_ms: int | None = field(default=None, init=False)
    _first_absence_ms: int | None = field(default=None, init=False)
    _change_candidate_ms: int | None = field(default=None, init=False)
    _patrol_counter: int = field(default=0, init=False)
    """自上次 patrol 以来经过的帧数（feat-031b）。"""

    def process(
        self,
        signature: FrameSignature,
        crop: np.ndarray | None = None,
    ) -> StateEvent | None:
        """处理帧签名，检测状态变化。

        Args:
            signature: 当前帧的签名。
            crop: 当前帧的裁剪图像（用于 SSIM 验证/patrol），可选。

        Returns:
            发生状态变化时返回 StateEvent，否则 None。
        """
        if self._state == State.EMPTY:
            event = self._process_empty(signature, crop)
        else:
            event = self._process_stable(signature, crop)

        self._last_signature = signature
        return event

    def _trace(
        self,
        signature: FrameSignature,
        *,
        event: StateEvent | None,
        trigger_reason: TriggerReason,
        veto_reason: VetoReason,
        distance: int | None = None,
        ssim: float | None = None,
    ) -> None:
        """记录一帧的决策上下文到 trace_recorder（如启用）。"""
        if self.trace_recorder is None:
            return
        recorder: TraceRecorder = self.trace_recorder
        has_subtitle = signature.foreground_ratio >= self.config.presence_threshold
        anchor_dhash = self._anchor_signature.dhash if self._anchor_signature else None
        recorder.record(
            timestamp_ms=signature.timestamp_ms,
            foreground_ratio=signature.foreground_ratio,
            has_subtitle=has_subtitle,
            dhash=signature.dhash,
            anchor_dhash=anchor_dhash,
            distance=distance,
            ssim=ssim,
            state=self._state.name,
            event_type=event.event_type.name if event else None,
            candidate_change_ms=self._change_candidate_ms,
            trigger_reason=trigger_reason,
            veto_reason=veto_reason,
        )

    def reset(self) -> None:
        """重置检测器状态。"""
        self._state = State.EMPTY
        self._anchor_signature = None
        self._anchor_crop = None
        self._last_signature = None
        self._consecutive_presence = 0
        self._consecutive_absence = 0
        self._stable_start_ms = None
        self._first_presence_ms = None
        self._first_absence_ms = None
        self._change_candidate_ms = None
        self._patrol_counter = 0

    @property
    def current_state(self) -> State:
        """获取当前状态。"""
        return self._state

    def _process_empty(
        self,
        signature: FrameSignature,
        crop: np.ndarray | None = None,
    ) -> StateEvent | None:
        """处理 EMPTY 状态。

        迁移条件：连续 N 帧前景占比 >= 阈值 → STABLE。
        """
        from sublift.diagnostics.trace import TriggerReason, VetoReason

        has_subtitle = signature.foreground_ratio >= self.config.presence_threshold

        if has_subtitle:
            if self._consecutive_presence == 0:
                self._first_presence_ms = signature.timestamp_ms
            self._consecutive_presence += 1
            self._consecutive_absence = 0

            if self._consecutive_presence >= self.config.hysteresis_frames:
                start_ms = self._first_presence_ms or signature.timestamp_ms
                self._state = State.STABLE
                self._anchor_signature = signature
                self._anchor_crop = crop
                self._stable_start_ms = start_ms
                self._consecutive_presence = 0
                self._first_presence_ms = None
                event = StateEvent(
                    event_type=EventType.IN,
                    timestamp_ms=start_ms,
                )
                self._trace(
                    signature,
                    event=event,
                    trigger_reason=TriggerReason.PRESENCE_RISE,
                    veto_reason=VetoReason.NONE,
                )
                return event
            self._trace(
                signature,
                event=None,
                trigger_reason=TriggerReason.NONE,
                veto_reason=VetoReason.HYSTERESIS_NOT_MET,
            )
        else:
            self._consecutive_presence = 0
            self._first_presence_ms = None
            self._trace(
                signature,
                event=None,
                trigger_reason=TriggerReason.NONE,
                veto_reason=VetoReason.NONE,
            )

        return None

    def _process_stable(
        self,
        signature: FrameSignature,
        crop: np.ndarray | None,
    ) -> StateEvent | None:
        """处理 STABLE / STABLE_PRIME 状态。

        迁移条件：
        - 连续 N 帧前景占比 < 阈值 → EMPTY
        - 与锚帧 dHash 距离 > 阈值 → 候选变化 → 稳定确认 → STABLE_PRIME
        """
        has_subtitle = signature.foreground_ratio >= self.config.presence_threshold

        if not has_subtitle:
            return self._handle_disappearance(signature)

        self._consecutive_absence = 0
        self._first_absence_ms = None

        if self._anchor_signature is not None:
            return self._handle_content_change(signature, crop)

        from sublift.diagnostics.trace import TriggerReason, VetoReason

        self._trace(
            signature,
            event=None,
            trigger_reason=TriggerReason.NONE,
            veto_reason=VetoReason.NONE,
        )
        return None

    def _handle_disappearance(
        self, signature: FrameSignature
    ) -> StateEvent | None:
        """处理字幕消失。"""
        from sublift.diagnostics.trace import TriggerReason, VetoReason

        if self._consecutive_absence == 0:
            self._first_absence_ms = signature.timestamp_ms
        self._consecutive_absence += 1

        if self._consecutive_absence >= self.config.hysteresis_frames:
            end_ms = self._first_absence_ms or signature.timestamp_ms
            self._state = State.EMPTY
            self._consecutive_absence = 0
            self._first_absence_ms = None
            self._stable_start_ms = None
            self._change_candidate_ms = None
            self._anchor_signature = None
            self._anchor_crop = None
            event = StateEvent(
                event_type=EventType.OUT,
                timestamp_ms=end_ms,
            )
            self._trace(
                signature,
                event=event,
                trigger_reason=TriggerReason.PRESENCE_FALL,
                veto_reason=VetoReason.NONE,
            )
            return event
        self._trace(
            signature,
            event=None,
            trigger_reason=TriggerReason.NONE,
            veto_reason=VetoReason.HYSTERESIS_NOT_MET,
        )
        return None

    def _handle_content_change(
        self,
        signature: FrameSignature,
        crop: np.ndarray | None,
    ) -> StateEvent | None:
        """处理字幕内容变化（含候选/稳定确认双阶段 + 可选 SSIM 验证/patrol）。"""
        from sublift.diagnostics.trace import TriggerReason, VetoReason

        assert self._anchor_signature is not None
        distance = hamming_distance(signature.dhash, self._anchor_signature.dhash)

        if distance <= self.config.change_threshold:
            return self._handle_patrol_path(signature, crop, distance)

        if self._is_ssim_vetoed(crop):
            self._change_candidate_ms = None
            self._trace(
                signature,
                event=None,
                trigger_reason=TriggerReason.DHASH_EXCEEDS,
                veto_reason=VetoReason.SSIM_VETOED,
                distance=distance,
            )
            return None

        if self._change_candidate_ms is None:
            self._change_candidate_ms = signature.timestamp_ms

        if self._is_new_content_stable(signature):
            change_ms = self._change_candidate_ms
            self._change_candidate_ms = None
            self._state = State.STABLE_PRIME
            self._anchor_signature = signature
            self._anchor_crop = crop
            self._stable_start_ms = change_ms
            event = StateEvent(
                event_type=EventType.CHANGE,
                timestamp_ms=change_ms,
                prev_end_ms=change_ms,
            )
            self._trace(
                signature,
                event=event,
                trigger_reason=TriggerReason.DHASH_EXCEEDS,
                veto_reason=VetoReason.NONE,
                distance=distance,
            )
            return event

        self._trace(
            signature,
            event=None,
            trigger_reason=TriggerReason.DHASH_EXCEEDS,
            veto_reason=VetoReason.UNSTABLE_CONTENT,
            distance=distance,
        )
        return None

    def _handle_patrol_path(
        self,
        signature: FrameSignature,
        crop: np.ndarray | None,
        distance: int,
    ) -> StateEvent | None:
        """dHash 未触发时的处理路径（feat-031b）。

        - 若 patrol 启用且存在未确认候选，走稳定确认流程
        - 周期性 patrol 检查 SSIM 结构变化，产生新候选
        - patrol 未启用时退化为原逻辑（返回 None）
        """
        from sublift.diagnostics.trace import TriggerReason, VetoReason

        self._patrol_counter += 1

        if (
            self.config.enable_ssim_patrol
            and self._change_candidate_ms is not None
            and self._is_new_content_stable(signature)
        ):
            change_ms = self._change_candidate_ms
            self._change_candidate_ms = None
            self._patrol_counter = 0
            self._state = State.STABLE_PRIME
            self._anchor_signature = signature
            self._anchor_crop = crop
            self._stable_start_ms = change_ms
            event = StateEvent(
                event_type=EventType.CHANGE,
                timestamp_ms=change_ms,
                prev_end_ms=change_ms,
            )
            self._trace(
                signature,
                event=event,
                trigger_reason=TriggerReason.SSIM_PATROL,
                veto_reason=VetoReason.NONE,
                distance=distance,
            )
            return event

        patrol_ssim = self._maybe_patrol(signature, crop)
        if (
            patrol_ssim is not None
            and patrol_ssim < self.config.ssim_patrol_threshold
            and self._change_candidate_ms is None
        ):
            self._change_candidate_ms = signature.timestamp_ms
            self._trace(
                signature,
                event=None,
                trigger_reason=TriggerReason.SSIM_PATROL,
                veto_reason=VetoReason.UNSTABLE_CONTENT,
                distance=distance,
                ssim=patrol_ssim,
            )
            return None

        self._trace(
            signature,
            event=None,
            trigger_reason=TriggerReason.NONE,
            veto_reason=VetoReason.DISTANCE_BELOW_THRESHOLD,
            distance=distance,
            ssim=patrol_ssim,
        )
        return None

    def _maybe_patrol(
        self,
        signature: FrameSignature,
        crop: np.ndarray | None,
    ) -> float | None:
        """周期性 SSIM 巡逻，返回 SSIM 值或 None（未到巡逻帧）。

        当 SSIM < threshold 时表示检测到结构变化，调用方据此产生候选。
        """
        if not self.config.enable_ssim_patrol:
            return None
        if self._patrol_counter < self.config.ssim_patrol_interval:
            return None
        if self._anchor_crop is None or crop is None:
            return None

        self._patrol_counter = 0
        return compute_foreground_ssim(
            crop,
            self._anchor_crop,
            use_mask=self.config.ssim_patrol_use_mask,
            window_size=self.config.ssim_window_size,
        )

    def _is_ssim_vetoed(self, crop: np.ndarray | None) -> bool:
        """SSIM 验证是否否决当前 dHash 变化候选。"""
        if not self.config.enable_ssim_verify:
            return False
        if self._anchor_crop is None or crop is None:
            return False
        ssim_val = compute_ssim(
            crop, self._anchor_crop, self.config.ssim_window_size
        )
        return ssim_val > self.config.ssim_threshold

    def _is_new_content_stable(self, signature: FrameSignature) -> bool:
        """判断新内容是否已稳定（与上一帧相似，不再是抖动）。"""
        if self._last_signature is None:
            return False
        last_distance = hamming_distance(signature.dhash, self._last_signature.dhash)
        return last_distance <= self.config.change_threshold
