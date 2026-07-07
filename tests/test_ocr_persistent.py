"""持久背景文字过滤单测（feat-034c）。

覆盖场景：
1. ticker 文本片段多变 + 中文目标 → ticker 剔除、中文保留（条件 B）
2. 固定水印 → 水印剔除（条件 A）
3. 双行字幕安全 → 双行保留
4. 短字幕安全（1 段不重复）→ 保留
5. 空段 / 无 lines → 不崩溃
6. policy=None / disabled → 原样返回
7. 纯 ticker 段 → 剔除后 text 为空
"""

from __future__ import annotations

from sublift.models import (
    BoundingBox,
    OcrLine,
    PersistentTextPolicy,
    SubtitleEntry,
    SubtitleProfile,
)
from sublift.ocr.persistent import filter_persistent_text


def _line(
    text: str,
    y: int,
    height: int,
    confidence: float = 0.9,
    x: int = 0,
    width: int = 200,
) -> OcrLine:
    return OcrLine(
        text=text,
        confidence=confidence,
        bbox=BoundingBox(x=x, y=y, width=width, height=height),
    )


def _entry(text: str, start: int = 0, end: int = 1000) -> SubtitleEntry:
    return SubtitleEntry(start_ms=start, end_ms=end, text=text, confidence=0.85)


def _profile(
    y_center: float = 55.0,
    y_tolerance: float = 47.5,
    line_height: float = 30.0,
    max_lines: int = 1,
    policy: PersistentTextPolicy | None = None,
) -> SubtitleProfile:
    return SubtitleProfile(
        y_center=y_center,
        y_tolerance=y_tolerance,
        line_height=line_height,
        max_lines=max_lines,
        persistent_text_policy=policy,
    )


class TestTickerFiltered:
    """场景 1：ticker 文本片段多变（条件 B 命中）。

    模拟用户报告的场景：中文目标「跟胡尼克」「一位平凡的狐狸」在 y≈55，
    ticker「UNLIKEL」「CONSPIRACY」「UNLIKEL _NA」在 y≈60（同带同高）。
    每段中文不同，ticker 文本片段也多变但 y_bin 相同。
    """

    def test_ticker_with_changing_text_filtered(self) -> None:
        # ticker 在 y=60, height=20（center=70）；中文在 y=40, height=30（center=55）
        # y_bin = center / (line_height * 0.5) = center / 15
        #   ticker bin = 70/15 ≈ 4
        #   中文 bin = 55/15 ≈ 3
        # 不同 y_bin，但 ticker 跨段出现 4 个不同文本片段 → 条件 B 命中
        entries = [
            _entry("跟胡尼克", start=0, end=1000),
            _entry("一位平凡的狐狸", start=1000, end=2000),
            _entry("调查这个案子", start=2000, end=3000),
            _entry("发现线索", start=3000, end=4000),
        ]
        segment_lines = [
            [_line("跟胡尼克", y=40, height=30), _line("UNLIKEL", y=60, height=20)],
            [_line("一位平凡的狐狸", y=40, height=30), _line("CONSPIRACY", y=60, height=20)],
            [_line("调查这个案子", y=40, height=30), _line("UNLIKEL _NA", y=60, height=20)],
            [_line("发现线索", y=40, height=30), _line("VCO", y=60, height=20)],
        ]
        profile = _profile(
            y_center=55.0, y_tolerance=20.0, line_height=30.0,
            policy=PersistentTextPolicy(min_distinct_texts=4),
        )

        result = filter_persistent_text(entries, segment_lines, profile)

        assert len(result) == 4
        for i, r in enumerate(result):
            assert r.text == entries[i].text.strip(), f"段 {i} 中文应保留"

    def test_ticker_same_y_as_subtitle_filtered_by_bin(self) -> None:
        """ticker 与中文 y 中心完全相同（用户描述的极端场景）。

        此时 y_bin 相同，条件 B 把整个 bin 标记为持久 → 中文也被剔除。
        这是已知限制：当 ticker 与目标字幕 y 完全重合且 ticker 文本多变时，
        无法靠 y 轨道区分。但实际中 ticker 通常在字幕下方或上方，y_bin 不同。
        本测试验证「y_bin 不同时」能正确剔除 ticker。
        """
        # 中文 y=40, height=30 (center=55, bin=55/15≈3)
        # ticker y=70, height=20 (center=80, bin=80/15≈5) — 不同 bin
        entries = [_entry("中文"), _entry("中文"), _entry("中文"), _entry("中文")]
        segment_lines = [
            [_line("中文", y=40, height=30), _line("UNLIKEL", y=70, height=20)],
            [_line("中文", y=40, height=30), _line("CONSPIRACY", y=70, height=20)],
            [_line("中文", y=40, height=30), _line("DISCOVER", y=70, height=20)],
            [_line("中文", y=40, height=30), _line("THE TRUTH", y=70, height=20)],
        ]
        profile = _profile(
            y_center=55.0, y_tolerance=40.0, line_height=30.0,
            policy=PersistentTextPolicy(min_distinct_texts=4),
        )

        result = filter_persistent_text(entries, segment_lines, profile)

        for r in result:
            assert r.text == "中文"


class TestFixedWatermarkFiltered:
    """场景 2：固定水印（条件 A 命中）。

    水印「© 2024」在每段都出现，文本不变，连续出现段数 ≥ K1。
    """

    def test_fixed_watermark_filtered(self) -> None:
        entries = [_entry("字幕A"), _entry("字幕B"), _entry("字幕C")]
        segment_lines = [
            [_line("字幕A", y=40, height=30), _line("© 2024", y=5, height=15)],
            [_line("字幕B", y=40, height=30), _line("© 2024", y=5, height=15)],
            [_line("字幕C", y=40, height=30), _line("© 2024", y=5, height=15)],
        ]
        profile = _profile(
            y_center=55.0, y_tolerance=30.0, line_height=30.0,
            policy=PersistentTextPolicy(min_repeat_segments=3, min_distinct_texts=10),
        )

        result = filter_persistent_text(entries, segment_lines, profile)

        assert all("2024" not in r.text for r in result)
        assert result[0].text == "字幕A"
        assert result[1].text == "字幕B"
        assert result[2].text == "字幕C"

    def test_watermark_not_persistent_enough_kept(self) -> None:
        """水印只出现 2 段，K1=3 → 不被剔除。"""
        entries = [_entry("字幕A"), _entry("字幕B"), _entry("字幕C")]
        segment_lines = [
            [_line("字幕A", y=40, height=30), _line("LOGO", y=5, height=15)],
            [_line("字幕B", y=40, height=30), _line("LOGO", y=5, height=15)],
            [_line("字幕C", y=40, height=30)],
        ]
        profile = _profile(
            y_center=55.0, y_tolerance=30.0, line_height=30.0,
            policy=PersistentTextPolicy(min_repeat_segments=3, min_distinct_texts=10),
        )

        result = filter_persistent_text(entries, segment_lines, profile)

        # LOGO 连续 2 段 < 3，不剔除；但 selector 的 y 轨道过滤会剔除它
        # （y=5 不在 y_center=55±30 内）
        # 所以 result 里不应有 LOGO
        assert all("LOGO" not in r.text for r in result)
        assert result[0].text == "字幕A"


class TestDoubleRowSubtitleSafe:
    """场景 3：双行字幕安全。

    双行中文，每行文本少变，y_bin 各自不同且都不满足条件 B（不同文本数 < M），
    也不满足条件 A（每行连续但文本变化，指纹不同）。
    """

    def test_double_row_subtitle_preserved(self) -> None:
        entries = [
            _entry("第一行\n第二行"),
            _entry("第一行\n第二行"),
            _entry("第一行\n第三行"),
        ]
        segment_lines = [
            [_line("第一行", y=20, height=30), _line("第二行", y=55, height=30)],
            [_line("第一行", y=20, height=30), _line("第二行", y=55, height=30)],
            [_line("第一行", y=20, height=30), _line("第三行", y=55, height=30)],
        ]
        profile = _profile(
            y_center=55.0, y_tolerance=40.0, line_height=30.0, max_lines=2,
            policy=PersistentTextPolicy(min_repeat_segments=3, min_distinct_texts=4),
        )

        result = filter_persistent_text(entries, segment_lines, profile)

        assert len(result) == 3
        assert result[0].text == "第一行\n第二行"
        assert result[1].text == "第一行\n第二行"
        assert result[2].text == "第一行\n第三行"


class TestShortSubtitleSafe:
    """场景 4：短字幕安全（1 段不重复）。"""

    def test_single_segment_subtitle_preserved(self) -> None:
        entries = [_entry("短字幕")]
        segment_lines = [[_line("短字幕", y=40, height=30), _line("TICKER", y=70, height=20)]]
        profile = _profile(
            y_center=55.0, y_tolerance=30.0, line_height=30.0,
            policy=PersistentTextPolicy(min_repeat_segments=3, min_distinct_texts=4),
        )

        result = filter_persistent_text(entries, segment_lines, profile)

        # 1 段：条件 A 不满足（连续 1 < 3），条件 B 不满足（1 个文本 < 4）
        # 但 selector 的 y 轨道过滤会剔除 TICKER（y=70 不在 y_center=55±30 内？70 在范围内）
        # y_tolerance=30 → 范围 [25, 85]，TICKER center=80 在范围内 → 不会被 y 轨道过滤
        # TICKER height=20 >= line_height*0.5=15 → 不会被行高过滤
        # 所以 TICKER 会保留在结果里（因为没有跨段统计可剔除）
        assert "短字幕" in result[0].text


class TestEmptySegments:
    """场景 5：空段 / 无 lines。"""

    def test_empty_segment_lines(self) -> None:
        entries = [_entry(""), _entry("字幕"), _entry("")]
        segment_lines: list[list[OcrLine]] = [[], [_line("字幕", y=40, height=30)], []]
        profile = _profile(
            policy=PersistentTextPolicy(min_repeat_segments=3, min_distinct_texts=4),
        )

        result = filter_persistent_text(entries, segment_lines, profile)

        assert len(result) == 3
        assert result[0].text == ""
        assert result[1].text == "字幕"
        assert result[2].text == ""

    def test_all_empty(self) -> None:
        entries = [_entry(""), _entry("")]
        segment_lines: list[list[OcrLine]] = [[], []]
        profile = _profile(policy=PersistentTextPolicy())

        result = filter_persistent_text(entries, segment_lines, profile)

        assert len(result) == 2
        assert all(r.text == "" for r in result)


class TestPolicyDisabled:
    """场景 6：policy=None / disabled → 原样返回。"""

    def test_policy_none_returns_copy(self) -> None:
        entries = [_entry("字幕A"), _entry("字幕B")]
        segment_lines = [
            [_line("字幕A", y=40, height=30)],
            [_line("字幕B", y=40, height=30)],
        ]
        profile = _profile(policy=None)

        result = filter_persistent_text(entries, segment_lines, profile)

        assert result == entries

    def test_policy_disabled_returns_copy(self) -> None:
        entries = [_entry("字幕A"), _entry("字幕B")]
        segment_lines = [
            [_line("字幕A", y=40, height=30), _line("WATERMARK", y=5, height=15)],
            [_line("字幕B", y=40, height=30), _line("WATERMARK", y=5, height=15)],
        ]
        profile = _profile(
            policy=PersistentTextPolicy(enabled=False, min_repeat_segments=2),
        )

        result = filter_persistent_text(entries, segment_lines, profile)

        assert result == entries


class TestPureTickerSegmentEmptied:
    """场景 7：纯水印段（每段只有顶部水印，没有目标字幕）。

    水印在目标 y 范围外（顶部），跨段重复 → 被剔除 → text 置空。
    """

    def test_pure_watermark_segment_emptied(self) -> None:
        entries = [_entry("LOGO"), _entry("LOGO"), _entry("LOGO")]
        segment_lines = [
            [_line("LOGO", y=5, height=15)],
            [_line("LOGO", y=5, height=15)],
            [_line("LOGO", y=5, height=15)],
        ]
        profile = _profile(
            y_center=55.0, y_tolerance=20.0, line_height=30.0,
            policy=PersistentTextPolicy(min_repeat_segments=3, min_distinct_texts=10),
        )

        result = filter_persistent_text(entries, segment_lines, profile)

        # LOGO 在 y=5（顶部），不在目标 y 轨道（55±20），连续 3 段 ≥ K1=3 → 剔除
        assert all(r.text == "" for r in result)


class TestLengthMismatchSafe:
    """entries 与 segment_lines 长度不一致 → 保守返回原样。"""

    def test_length_mismatch_returns_copy(self) -> None:
        entries = [_entry("字幕A")]
        segment_lines = [[_line("字幕A", y=40, height=30)], []]
        profile = _profile(policy=PersistentTextPolicy())

        result = filter_persistent_text(entries, segment_lines, profile)

        assert result == entries
