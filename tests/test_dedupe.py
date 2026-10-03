"""去重合并（dedupe）测试。

纯单元测试，合成 SubtitleEntry，不依赖外部资源。
"""

from __future__ import annotations

from sublift.models import SubtitleEntry
from sublift.pipeline.dedupe import merge_entries


def _entry(start: int, end: int, text: str) -> SubtitleEntry:
    return SubtitleEntry(start_ms=start, end_ms=end, text=text)


class TestMergeAdjacent:
    """连续相同合并。"""

    def test_merge_same_adjacent(self) -> None:
        """相邻相同文本 + gap 合法 → 合并。"""
        entries = [
            _entry(0, 1000, "你好"),
            _entry(1100, 2000, "你好"),
        ]
        result = merge_entries(entries, merge_gap_ms=1000, min_duration_ms=0)
        assert result == [_entry(0, 2000, "你好")]

    def test_no_merge_gap_exceeds(self) -> None:
        """gap 超限 → 不合并。"""
        entries = [
            _entry(0, 1000, "你好"),
            _entry(3000, 4000, "你好"),
        ]
        result = merge_entries(entries, merge_gap_ms=1000, min_duration_ms=0)
        assert len(result) == 2

    def test_no_merge_different_text(self) -> None:
        """文本不同 → 不合并。"""
        entries = [
            _entry(0, 1000, "你好"),
            _entry(1000, 2000, "再见"),
        ]
        result = merge_entries(entries, merge_gap_ms=1000, min_duration_ms=0)
        assert len(result) == 2

    def test_merge_preserves_first_text(self) -> None:
        """合并后保留第一段文本。"""
        entries = [
            _entry(0, 1000, "你好"),
            _entry(1000, 2000, "你好"),
        ]
        result = merge_entries(entries, merge_gap_ms=1000, min_duration_ms=0)
        assert result[0].text == "你好"

    def test_merge_whitespace_normalized(self) -> None:
        """空白差异归一化后相等 → 合并，文本保留第一段。"""
        entries = [
            _entry(0, 1000, "你好"),
            _entry(1000, 2000, " 你 好 "),
        ]
        result = merge_entries(entries, merge_gap_ms=1000, min_duration_ms=0)
        assert len(result) == 1
        assert result[0].text == "你好"

    def test_merge_chain(self) -> None:
        """链式合并：A+A+A → 一段。"""
        entries = [
            _entry(0, 1000, "你好"),
            _entry(1000, 2000, "你好"),
            _entry(2000, 3000, "你好"),
        ]
        result = merge_entries(entries, merge_gap_ms=1000, min_duration_ms=0)
        assert result == [_entry(0, 3000, "你好")]


class TestFilterShort:
    """过滤过短段。"""

    def test_filter_short_segment(self) -> None:
        """duration < min_duration_ms → 丢弃。"""
        entries = [_entry(0, 300, "噪点")]
        result = merge_entries(entries, merge_gap_ms=1000, min_duration_ms=500)
        assert result == []

    def test_keep_valid_segment(self) -> None:
        """duration >= min_duration_ms → 保留。"""
        entries = [_entry(0, 1000, "你好")]
        result = merge_entries(entries, merge_gap_ms=1000, min_duration_ms=500)
        assert result == [_entry(0, 1000, "你好")]


class TestFilterEmpty:
    """过滤空文本段（drop_empty_text=True 旧行为）。"""

    def test_filter_empty_text(self) -> None:
        """text 为空字符串 → 丢弃。"""
        entries = [_entry(0, 1000, "")]
        result = merge_entries(entries, merge_gap_ms=1000, min_duration_ms=0, drop_empty_text=True)
        assert result == []

    def test_filter_whitespace_only(self) -> None:
        """text 仅含空白 → 丢弃。"""
        entries = [_entry(0, 1000, "   \n\t  ")]
        result = merge_entries(entries, merge_gap_ms=1000, min_duration_ms=0, drop_empty_text=True)
        assert result == []

    def test_keep_nonempty_around_empty(self) -> None:
        """空段两侧的有效段不合并（文本不同），仅空段被丢弃。"""
        entries = [
            _entry(0, 1000, "你好"),
            _entry(1000, 2000, ""),
            _entry(2000, 3000, "再见"),
        ]
        result = merge_entries(entries, merge_gap_ms=1000, min_duration_ms=0, drop_empty_text=True)
        assert result == [
            _entry(0, 1000, "你好"),
            _entry(2000, 3000, "再见"),
        ]

    def test_empty_between_same_then_merge(self) -> None:
        """空段两侧相同段：空段丢弃后两侧变相邻 → 合并。"""
        entries = [
            _entry(0, 1000, "你好"),
            _entry(1000, 2000, ""),
            _entry(2000, 3000, "你好"),
        ]
        result = merge_entries(entries, merge_gap_ms=1000, min_duration_ms=0, drop_empty_text=True)
        assert result == [_entry(0, 3000, "你好")]

    def test_all_empty(self) -> None:
        """全部为空段 → 返回空列表。"""
        entries = [
            _entry(0, 1000, ""),
            _entry(1000, 2000, "  "),
        ]
        result = merge_entries(entries, merge_gap_ms=1000, min_duration_ms=0, drop_empty_text=True)
        assert result == []

    def test_keep_empty_by_default(self) -> None:
        """feat-033b：默认保留空文本段，避免抹掉时间轴。"""
        entries = [
            _entry(0, 1000, "你好"),
            _entry(1000, 2000, ""),
            _entry(2000, 3000, "再见"),
        ]
        result = merge_entries(entries, merge_gap_ms=1000, min_duration_ms=0)
        assert len(result) == 3
        assert result[1].text == ""

    def test_consecutive_empty_kept_independent(self) -> None:
        """相邻空文本不能合并，应保持独立。"""
        entries = [
            _entry(0, 1000, ""),
            _entry(1000, 2000, ""),
            _entry(2000, 3000, ""),
        ]
        result = merge_entries(entries, merge_gap_ms=1000, min_duration_ms=0)
        assert len(result) == 3
        assert result[0].end_ms == 1000
        assert result[1].end_ms == 2000

    def test_empty_between_same_merged_even_when_keeping_empty(self) -> None:
        """默认保留空段时，夹在两句相同字幕中间的空 OCR 失败段仍应被合并掉。"""
        entries = [
            _entry(0, 4200, "放开我"),
            _entry(4200, 5000, ""),
            _entry(5000, 5800, "放开我"),
        ]
        result = merge_entries(entries, merge_gap_ms=1000, min_duration_ms=500)
        assert result == [_entry(0, 5800, "放开我")]


class TestJitterElimination:
    """抖动消除：A→B(短)→A → A。"""

    def test_jitter_short_middle(self) -> None:
        """中间短段过滤后，两侧相同段合并。"""
        entries = [
            _entry(0, 1000, "你好"),
            _entry(1000, 1100, "你妳"),  # 100ms 误识，会被过滤
            _entry(1100, 2000, "你好"),
        ]
        result = merge_entries(entries, merge_gap_ms=1000, min_duration_ms=500)
        assert result == [_entry(0, 2000, "你好")]

    def test_two_short_same_merge_then_keep(self) -> None:
        """两个短相同段合并后变合法 → 保留（3-pass 的关键 case）。"""
        entries = [
            _entry(0, 300, "你好"),  # 300ms < 500，单独会被删
            _entry(400, 700, "你好"),  # 300ms < 500，单独会被删
        ]
        result = merge_entries(entries, merge_gap_ms=1000, min_duration_ms=500)
        assert result == [_entry(0, 700, "你好")]


class TestEdgeCases:
    """边界情况。"""

    def test_empty_input(self) -> None:
        assert merge_entries([], merge_gap_ms=1000, min_duration_ms=500) == []

    def test_single_entry_kept(self) -> None:
        entries = [_entry(0, 1000, "你好")]
        result = merge_entries(entries, merge_gap_ms=1000, min_duration_ms=500)
        assert result == [_entry(0, 1000, "你好")]

    def test_single_entry_filtered(self) -> None:
        entries = [_entry(0, 200, "噪点")]
        result = merge_entries(entries, merge_gap_ms=1000, min_duration_ms=500)
        assert result == []

    def test_input_not_mutated(self) -> None:
        """不改变输入列表。"""
        entries = [_entry(0, 1000, "你好"), _entry(1100, 2000, "你好")]
        original = list(entries)
        merge_entries(entries, merge_gap_ms=1000, min_duration_ms=0)
        assert entries == original


class TestComplexScenarios:
    """复杂场景。"""

    def test_mixed_sequence(self) -> None:
        """混合序列：长段 + 短噪声 + 长段 + 不同长段。"""
        entries = [
            _entry(0, 1000, "你好"),
            _entry(1000, 1100, "你妳"),  # 短噪声
            _entry(1100, 2000, "你好"),
            _entry(2000, 3000, "再见"),
        ]
        result = merge_entries(entries, merge_gap_ms=1000, min_duration_ms=500)
        assert result == [
            _entry(0, 2000, "你好"),
            _entry(2000, 3000, "再见"),
        ]

    def test_three_pass_needed(self) -> None:
        """需要 3-pass 的场景：A(短)+B(短)+A(短) 链。"""
        entries = [
            _entry(0, 300, "你好"),
            _entry(400, 700, "你好"),
            _entry(800, 1100, "你好"),
        ]
        result = merge_entries(entries, merge_gap_ms=1000, min_duration_ms=500)
        assert result == [_entry(0, 1100, "你好")]


class TestGrowingOcrFragments:
    """同一句逐步出字 / 包含关系应合并成一句。"""

    def test_typewriter_watermark(self) -> None:
        entries = [
            _entry(0, 600, "更"),
            _entry(600, 1400, "多"),
            _entry(1400, 3200, "多资源尽在98堂"),
            _entry(3400, 5000, "更多资源尽在98堂"),
            _entry(5000, 6600, "更多资源尽在98堂永久地址："),
            _entry(6600, 7400, "更多资源尽在98堂永久地址：WWW.98T.LA"),
            _entry(8000, 8800, "更多资源尽在98堂永久地址："),
            _entry(8800, 9400, "更多资源尽在98堂"),
        ]
        result = merge_entries(entries, merge_gap_ms=1000, min_duration_ms=0)
        assert len(result) == 1
        assert result[0].start_ms == 0
        assert result[0].end_ms == 9400
        assert "WWW.98T.LA" in result[0].text

    def test_one_char_not_suffix_absorb(self) -> None:
        """单字若不是长句前缀，不吞并（避免「嘛」并进下一句）。"""
        entries = [
            _entry(0, 800, "嘛"),
            _entry(800, 2800, "知道啦进公司后就禁止"),
        ]
        result = merge_entries(entries, merge_gap_ms=1000, min_duration_ms=0)
        assert len(result) == 2

    def test_containment_keeps_longer(self) -> None:
        entries = [
            _entry(0, 1000, "不是挺好"),
            _entry(1000, 2000, "这不是挺好嘛能吃到不少美食哦"),
        ]
        result = merge_entries(entries, merge_gap_ms=1000, min_duration_ms=0)
        assert len(result) == 1
        assert result[0].text == "这不是挺好嘛能吃到不少美食哦"
