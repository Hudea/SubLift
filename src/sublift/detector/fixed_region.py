"""固定区域检测器：使用手动指定的字幕区域。"""

from __future__ import annotations

from sublift.models import BoundingBox, Frame, Region


class FixedRegionDetector:
    """使用手动指定的固定字幕区域。

    忽略帧实际尺寸，原样返回构造时指定的 BoundingBox。越界检查由下游负责。
    """

    def __init__(self, region: BoundingBox) -> None:
        """初始化固定区域检测器。

        Args:
            region: 手动指定的字幕区域（绝对像素坐标）。
        """
        self._region = region

    def detect(self, frame: Frame) -> Region:
        """返回固定的字幕区域。

        Args:
            frame: 视频帧（仅用于满足 Protocol 签名，不参与计算）。

        Returns:
            构造时指定的字幕区域 Region。
        """
        return Region(box=self._region)
