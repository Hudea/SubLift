"""ROI 局部全幅透传检测器（frame-local 坐标）。

只消费 ROI 输出图的宽高，返回 ``[0, 0, width, height]``。
禁止接收 source-frame 的 ``source_region_box``：那会把 source Y
再次应用到局部图，造成二次裁剪或越界（见 docs/design/roi-data-path.md）。
"""

from __future__ import annotations

from sublift.models import BoundingBox, Frame, Region


class RoiPassthroughDetector:
    """对 ROI Frame 返回局部全幅 Region。

    构造参数仅是 frame-local 尺寸（即 extractor 输出图宽高），
    与 source-frame 选区坐标无关。
    """

    def __init__(self, width: int, height: int) -> None:
        """初始化 ROI 透传检测器。

        Args:
            width: ROI 图像宽度（像素，frame-local）。
            height: ROI 图像高度（像素，frame-local）。

        Raises:
            ValueError: width/height 非正。
        """
        if width <= 0 or height <= 0:
            raise ValueError(
                f"RoiPassthroughDetector 需要正尺寸，收到 width={width} height={height}"
            )
        self._box = BoundingBox(x=0, y=0, width=width, height=height)

    @property
    def box(self) -> BoundingBox:
        """frame-local 全幅区域。"""
        return self._box

    def detect(self, frame: Frame) -> Region:
        """返回局部全幅 Region。

        Args:
            frame: 当前帧（满足 Protocol；不参与 box 计算）。

        Returns:
            ``Region(box=[0, 0, width, height])``。
        """
        return Region(box=self._box)
