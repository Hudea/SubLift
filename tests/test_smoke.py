"""SubLift 冒烟测试。

验证核心数据模型与 CLI 解析的基础行为，不依赖外部资源（视频/ffmpeg/Vision）。
"""

from __future__ import annotations

import pytest

from sublift.cli import build_parser
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


class TestCli:
    """CLI 参数解析测试。"""

    def test_extract_parses_required_args(self) -> None:
        parser = build_parser()
        args = parser.parse_args(["extract", "video.mp4"])
        assert args.command == "extract"
        assert args.video == "video.mp4"
        assert args.output == "output.srt"

    def test_extract_parses_output_option(self) -> None:
        parser = build_parser()
        args = parser.parse_args(["extract", "video.mp4", "-o", "out.srt"])
        assert args.output == "out.srt"

    def test_extract_default_fps(self) -> None:
        parser = build_parser()
        args = parser.parse_args(["extract", "video.mp4"])
        assert args.fps == 1.0

    def test_extract_custom_fps(self) -> None:
        parser = build_parser()
        args = parser.parse_args(["extract", "video.mp4", "--fps", "2.0"])
        assert args.fps == 2.0

    def test_extract_default_confidence(self) -> None:
        parser = build_parser()
        args = parser.parse_args(["extract", "video.mp4"])
        assert args.confidence == 0.5

    def test_no_command_sets_none(self) -> None:
        parser = build_parser()
        args = parser.parse_args([])
        assert args.command is None
