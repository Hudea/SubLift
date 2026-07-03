"""字幕区域检测能力模块。"""

from sublift.detector.base import Detector
from sublift.detector.bottom_crop import BottomCropDetector
from sublift.detector.fixed_region import FixedRegionDetector

__all__ = ["BottomCropDetector", "Detector", "FixedRegionDetector"]
