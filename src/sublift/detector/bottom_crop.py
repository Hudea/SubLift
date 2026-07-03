"""下部裁剪检测器：按比例裁剪画面下部作为字幕区域。"""

from __future__ import annotations

from sublift.models import BoundingBox, Frame, Region


class BottomCropDetector:
    """按比例裁剪画面下部确定字幕区域。

    默认裁剪下部 30%，可通过 bottom_ratio 配置。越界检查由下游负责。
    """

    def __init__(self, bottom_ratio: float = 0.3) -> None:
        """初始化下部裁剪检测器。

        Args:
            bottom_ratio: 裁剪比例（0.0~1.0），默认 0.3（下部 30%）。
                0.0 返回 height=0 的空区域。
        """
        self._bottom_ratio = bottom_ratio

    def detect(self, frame: Frame) -> Region:
        """按比例计算画面下部的字幕区域。

        Args:
            frame: 视频帧。

        Returns:
            字幕区域 Region，绝对像素坐标。
        """
        width, height = frame.image.size
        crop_height = int(height * self._bottom_ratio)
        box = BoundingBox(
            x=0,
            y=height - crop_height,
            width=width,
            height=crop_height,
        )
        return Region(box=box)
