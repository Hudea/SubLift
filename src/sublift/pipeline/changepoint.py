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

import numpy as np

from sublift.config import ChangePointConfig
from sublift.pipeline.signature import (
    FrameSignature,
    compute_ssim,
    hamming_distance,
)


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

    def process(
        self,
        signature: FrameSignature,
        crop: np.ndarray | None = None,
    ) -> StateEvent | None:
        """处理帧签名，检测状态变化。

        Args:
            signature: 当前帧的签名。
            crop: 当前帧的裁剪图像（用于 SSIM 验证），可选。

        Returns:
            发生状态变化时返回 StateEvent，否则 None。
        """
        event: StateEvent | None = None
        if self._state == State.EMPTY:
            event = self._process_empty(signature)
        else:
            event = self._process_stable(signature, crop)

        self._last_signature = signature
        return event

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

    @property
    def current_state(self) -> State:
        """获取当前状态。"""
        return self._state

    def _process_empty(self, signature: FrameSignature) -> StateEvent | None:
        """处理 EMPTY 状态。

        迁移条件：连续 N 帧前景占比 >= 阈值 → STABLE。
        """
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
                self._stable_start_ms = start_ms
                self._consecutive_presence = 0
                self._first_presence_ms = None
                return StateEvent(
                    event_type=EventType.IN,
                    timestamp_ms=start_ms,
                )
        else:
            self._consecutive_presence = 0
            self._first_presence_ms = None

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

        return None

    def _handle_disappearance(
        self, signature: FrameSignature
    ) -> StateEvent | None:
        """处理字幕消失。"""
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
            return StateEvent(
                event_type=EventType.OUT,
                timestamp_ms=end_ms,
            )
        return None

    def _handle_content_change(
        self,
        signature: FrameSignature,
        crop: np.ndarray | None,
    ) -> StateEvent | None:
        """处理字幕内容变化（含候选/稳定确认双阶段 + 可选 SSIM 验证）。"""
        assert self._anchor_signature is not None
        distance = hamming_distance(signature.dhash, self._anchor_signature.dhash)

        if distance <= self.config.change_threshold:
            if self._change_candidate_ms is not None:
                self._change_candidate_ms = None
            return None

        if self._is_ssim_vetoed(crop):
            self._change_candidate_ms = None
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
            return StateEvent(
                event_type=EventType.CHANGE,
                timestamp_ms=change_ms,
                prev_end_ms=change_ms,
            )

        return None

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
