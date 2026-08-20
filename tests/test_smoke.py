"""SubLift 冒烟测试。

验证离线工具包数据模型的基础行为，不依赖外部资源。
"""

from __future__ import annotations

import pytest

from sublift.models import BoundingBox, Frame, OcrResult, Region, SubtitleEntry


class TestModels:
    """核心数据模型测试。"""

    def test_bounding_box(self) -> None:
        box = BoundingBox(x=0, y=720, width=1920, height=360)
        assert box.x == 0
        assert box.width == 1920

    def test_region(self) -> None:
        box = BoundingBox(x=0, y=720, width=1920, height=360)
        region = Region(box=box)
        assert region.box is box

    def test_frame_timestamp(self) -> None:
        frame = Frame(timestamp_ms=1000, image=None)  # type: ignore[arg-type]
        assert frame.timestamp_ms == 1000

    def test_ocr_result(self) -> None:
        result = OcrResult(text="你好", confidence=0.95)
        assert result.text == "你好"
        assert 0.0 <= result.confidence <= 1.0

    def test_subtitle_entry(self) -> None:
        entry = SubtitleEntry(start_ms=1000, end_ms=2000, text="测试")
        assert entry.end_ms - entry.start_ms == 1000

    def test_models_are_frozen(self) -> None:
        entry = SubtitleEntry(start_ms=0, end_ms=100, text="x")
        with pytest.raises(AttributeError):
            entry.text = "y"  # type: ignore[misc]



