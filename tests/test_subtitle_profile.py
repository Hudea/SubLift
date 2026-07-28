"""feat-034b：SubtitleProfile 推导与序列化。"""

from __future__ import annotations

import pytest
from PIL import Image

from sublift.config import Config
from sublift.detector.fixed_region import FixedRegionDetector
from sublift.models import SCRIPT_AUTO, BoundingBox, Frame, SubtitleProfile
from sublift.ocr.mock import MockOcrEngine
from sublift.pipeline.core import Pipeline


class TestSubtitleProfileFromCrop:
    def test_fills_band(self) -> None:
        p = SubtitleProfile.from_crop(1920, 87)
        assert p.script == SCRIPT_AUTO
        assert p.center_x == 960
        assert p.center_y == 43
        assert p.height == 87
        assert p.y_min == 0
        assert p.y_max == 87

    def test_zero_size(self) -> None:
        p = SubtitleProfile.from_crop(0, 0)
        assert p.center_x == 0
        assert p.height == 0


class TestSubtitleProfileFromSelection:
    def test_maps_video_selection_into_crop(self) -> None:
        # crop: full width band at y=800 h=100
        region = BoundingBox(x=0, y=800, width=1920, height=100)
        # selection: narrow box inside band
        selection = BoundingBox(x=400, y=820, width=200, height=40)
        p = SubtitleProfile.from_selection_in_video(region, selection)
        assert p.center_x == 500  # 400 + 200//2 - 0
        assert p.center_y == 40  # 820 - 800 + 20
        assert p.height == 40
        assert p.y_min == 20
        assert p.y_max == 60

    def test_clamps_outside_crop(self) -> None:
        region = BoundingBox(x=0, y=100, width=100, height=50)
        selection = BoundingBox(x=0, y=0, width=100, height=200)  # spills
        p = SubtitleProfile.from_selection_in_video(region, selection)
        assert p.y_min == 0
        assert p.y_max == 50
        assert p.height == 50


class TestSubtitleProfileDict:
    def test_roundtrip(self) -> None:
        p = SubtitleProfile(
            script="latin",
            center_x=10,
            center_y=20,
            height=30,
            y_min=5,
            y_max=35,
        )
        assert SubtitleProfile.from_dict(p.to_dict()) == p

    def test_invalid_script(self) -> None:
        with pytest.raises(ValueError, match="script"):
            SubtitleProfile.from_dict(
                {
                    "script": "emoji",
                    "center_x": 0,
                    "center_y": 0,
                    "height": 1,
                    "y_min": 0,
                    "y_max": 1,
                }
            )

    def test_missing_field(self) -> None:
        with pytest.raises(ValueError, match="center_x"):
            SubtitleProfile.from_dict({"script": "cjk"})


class TestPipelineProfile:
    def test_explicit_profile_retained(self) -> None:
        profile = SubtitleProfile.from_crop(320, 80)
        region = BoundingBox(x=0, y=100, width=320, height=80)
        detector = FixedRegionDetector(region)
        config = Config(subtitle_profile=profile)
        pipe = Pipeline(detector=detector, ocr=MockOcrEngine(), config=config)
        img = Image.new("RGB", (320, 200), "white")
        pipe.feed(Frame(timestamp_ms=0, image=img))
        assert pipe.subtitle_profile == profile

    def test_derives_from_region_when_missing(self) -> None:
        region = BoundingBox(x=0, y=100, width=320, height=80)
        detector = FixedRegionDetector(region)
        pipe = Pipeline(detector=detector, ocr=MockOcrEngine(), config=Config())
        assert pipe.subtitle_profile is None
        img = Image.new("RGB", (320, 200), "white")
        pipe.feed(Frame(timestamp_ms=0, image=img))
        assert pipe.subtitle_profile == SubtitleProfile.from_crop(320, 80)

    def test_derived_auto_profile_keeps_english(self) -> None:
        region = BoundingBox(x=0, y=100, width=320, height=80)
        detector = FixedRegionDetector(region)
        pipe = Pipeline(
            detector=detector,
            ocr=MockOcrEngine(text="HELLO", confidence=0.9),
            config=Config(min_duration_ms=0),
        )
        img = Image.new("RGB", (320, 200), "white")
        pipe.feed(Frame(timestamp_ms=0, image=img))
        assert pipe.subtitle_profile is not None
        assert pipe.subtitle_profile.script == SCRIPT_AUTO
