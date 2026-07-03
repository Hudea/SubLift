"""字幕区域检测抽象接口。"""

from __future__ import annotations

from typing import Protocol

from sublift.models import Frame, Region


class Detector(Protocol):
    """确定字幕所在区域。"""

    def detect(self, frame: Frame) -> Region:
        """检测给定帧中的字幕区域。

        Args:
            frame: 视频帧。

        Returns:
            字幕区域 Region。
        """
        ...
