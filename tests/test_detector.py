"""字幕区域检测模块（detector）测试。

纯单元测试，用 PIL.Image.new 造帧，不依赖外部资源。
"""

from __future__ import annotations

from PIL import Image

from sublift.detector.base import Detector
from sublift.detector.bottom_crop import BottomCropDetector
from sublift.detector.fixed_region import FixedRegionDetector
from sublift.models import BoundingBox, Frame


def _make_frame(width: int, height: int) -> Frame:
    """造一帧纯色图像用于测试。"""
    image = Image.new("RGB", (width, height))
    return Frame(timestamp_ms=0, image=image)


class TestBottomCropDetector:
    """BottomCropDetector 单测。"""

    def test_is_detector(self) -> None:
        detector = BottomCropDetector()
        assert isinstance(detector, Detector)

    def test_default_ratio_1080p(self) -> None:
        """默认 0.3 比例：1920x1080 → box(0, 756, 1920, 324)。"""
        detector = BottomCropDetector()
        frame = _make_frame(1920, 1080)
        region = detector.detect(frame)
        assert region.box == BoundingBox(x=0, y=756, width=1920, height=324)

    def test_custom_ratio(self) -> None:
        """自定义 0.25 比例：1920x1080 → box(0, 810, 1920, 270)。"""
        detector = BottomCropDetector(bottom_ratio=0.25)
        frame = _make_frame(1920, 1080)
        region = detector.detect(frame)
        assert region.box == BoundingBox(x=0, y=810, width=1920, height=270)

    def test_ratio_full(self) -> None:
        """ratio=1.0：整帧。"""
        detector = BottomCropDetector(bottom_ratio=1.0)
        frame = _make_frame(1920, 1080)
        region = detector.detect(frame)
        assert region.box == BoundingBox(x=0, y=0, width=1920, height=1080)

    def test_ratio_zero(self) -> None:
        """ratio=0.0：height=0 空区域。"""
        detector = BottomCropDetector(bottom_ratio=0.0)
        frame = _make_frame(1920, 1080)
        region = detector.detect(frame)
        assert region.box == BoundingBox(x=0, y=1080, width=1920, height=0)

    def test_non_standard_size(self) -> None:
        """非标准尺寸 320x240，默认 0.3 → box(0, 168, 320, 72)。"""
        detector = BottomCropDetector()
        frame = _make_frame(320, 240)
        region = detector.detect(frame)
        assert region.box == BoundingBox(x=0, y=168, width=320, height=72)


class TestFixedRegionDetector:
    """FixedRegionDetector 单测。"""

    def test_is_detector(self) -> None:
        detector = FixedRegionDetector(BoundingBox(0, 0, 100, 50))
        assert isinstance(detector, Detector)

    def test_returns_fixed_region(self) -> None:
        """原样返回构造时指定的区域。"""
        region_box = BoundingBox(x=10, y=20, width=100, height=50)
        detector = FixedRegionDetector(region_box)
        frame = _make_frame(1920, 1080)
        region = detector.detect(frame)
        assert region.box == region_box

    def test_ignores_frame_size(self) -> None:
        """不同帧尺寸返回相同区域。"""
        region_box = BoundingBox(x=10, y=20, width=100, height=50)
        detector = FixedRegionDetector(region_box)
        frame_small = _make_frame(320, 240)
        frame_large = _make_frame(1920, 1080)
        assert detector.detect(frame_small).box == region_box
        assert detector.detect(frame_large).box == region_box
