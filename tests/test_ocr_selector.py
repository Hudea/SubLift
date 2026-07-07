"""字幕行 selector 单测（feat-033c）。

4 个核心场景：
1. 单行中文 + 背景英文标题（不同 y）→ 只选中文字幕
2. 双行中文字幕 → 保留两行
3. 单行中文 + 顶部水印 → 过滤水印
4. 空行 / 无匹配 → 返回空
"""

from __future__ import annotations

from sublift.models import BoundingBox, OcrLine, SubtitleProfile
from sublift.ocr.selector import select_lines


def _line(
    text: str,
    y: int,
    height: int,
    confidence: float = 0.9,
    x: int = 0,
    width: int = 200,
) -> OcrLine:
    """构造测试用 OcrLine。"""
    return OcrLine(
        text=text,
        confidence=confidence,
        bbox=BoundingBox(x=x, y=y, width=width, height=height),
    )


class TestSelectLinesSingleRow:
    """场景 1：单行中文字幕 + 背景英文标题。"""

    def test_filters_background_english(self) -> None:
        """目标中文字幕在 y=900，英文标题在 y=100，profile 只选中文字幕。"""
        target = _line("你好世界", y=900, height=50, confidence=0.95)
        background = _line("CHAPTER 1", y=100, height=30, confidence=0.8)
        profile = SubtitleProfile(
            y_center=925.0,
            y_tolerance=40.0,
            line_height=40.0,
            max_lines=1,
        )
        text, conf = select_lines([target, background], profile)
        assert text == "你好世界"
        assert conf == 0.95

    def test_picks_correct_y_band(self) -> None:
        """多层文字，只选 y 在容差范围内的。"""
        top = _line("LOGO", y=50, height=20)
        middle = _line("广告", y=400, height=30)
        bottom = _line("字幕", y=950, height=50)
        profile = SubtitleProfile(
            y_center=975.0,
            y_tolerance=50.0,
            line_height=40.0,
            max_lines=1,
        )
        text, _ = select_lines([top, middle, bottom], profile)
        assert text == "字幕"


class TestSelectLinesDoubleRow:
    """场景 2：双行中文字幕。"""

    def test_keeps_two_lines(self) -> None:
        """双行字幕，max_lines=2，保留两行并按 y 排序。"""
        line1 = _line("第一行", y=880, height=45, confidence=0.9)
        line2 = _line("第二行", y=940, height=45, confidence=0.85)
        profile = SubtitleProfile(
            y_center=935.0,
            y_tolerance=80.0,
            line_height=40.0,
            max_lines=2,
        )
        text, conf = select_lines([line2, line1], profile)
        assert text == "第一行\n第二行"
        assert abs(conf - 0.875) < 0.01

    def test_max_lines_truncation_keeps_highest_confidence(self) -> None:
        """3 行都匹配，max_lines=2 → 保留置信度最高的 2 行。"""
        a = _line("A", y=880, height=45, confidence=0.7)
        b = _line("B", y=920, height=45, confidence=0.95)
        c = _line("C", y=960, height=45, confidence=0.85)
        profile = SubtitleProfile(
            y_center=920.0,
            y_tolerance=80.0,
            line_height=40.0,
            max_lines=2,
        )
        text, _ = select_lines([a, b, c], profile)
        lines = text.split("\n")
        assert set(lines) == {"B", "C"}


class TestSelectLinesWatermark:
    """场景 3：单行中文 + 顶部水印。"""

    def test_filters_small_watermark(self) -> None:
        """水印行高过小，被 line_height 过滤。"""
        subtitle = _line("正文", y=900, height=50, confidence=0.9)
        watermark = _line("© 2024", y=900, height=15, confidence=0.7)
        profile = SubtitleProfile(
            y_center=925.0,
            y_tolerance=40.0,
            line_height=40.0,
            max_lines=1,
        )
        text, _ = select_lines([subtitle, watermark], profile)
        assert text == "正文"

    def test_filters_out_of_band_watermark(self) -> None:
        """水印 y 在容差范围外。"""
        subtitle = _line("正文", y=900, height=50, confidence=0.9)
        watermark = _line("WATERMARK", y=500, height=20, confidence=0.6)
        profile = SubtitleProfile(
            y_center=925.0,
            y_tolerance=40.0,
            line_height=40.0,
            max_lines=1,
        )
        text, _ = select_lines([subtitle, watermark], profile)
        assert text == "正文"


class TestSelectLinesEmpty:
    """场景 4：空行 / 无匹配。"""

    def test_no_lines(self) -> None:
        profile = SubtitleProfile(
            y_center=100.0, y_tolerance=20.0, line_height=40.0
        )
        text, conf = select_lines([], profile)
        assert text == ""
        assert conf == 0.0

    def test_no_y_match(self) -> None:
        line = _line("x", y=500, height=40)
        profile = SubtitleProfile(
            y_center=900.0, y_tolerance=20.0, line_height=40.0
        )
        text, conf = select_lines([line], profile)
        assert text == ""
        assert conf == 0.0

    def test_no_height_match(self) -> None:
        """y 匹配但行高过低。"""
        line = _line("x", y=890, height=10)
        profile = SubtitleProfile(
            y_center=900.0, y_tolerance=30.0, line_height=40.0
        )
        text, conf = select_lines([line], profile)
        assert text == ""
        assert conf == 0.0
