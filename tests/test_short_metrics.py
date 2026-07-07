"""短字幕指标测试（feat-031c）。

用合成的检测段与 ground truth 验证短字幕召回率计算。
"""

from __future__ import annotations

import pytest

pytest.importorskip("benchmark.srt_loader")
from benchmark.srt_loader import SrtEntry

from sublift.diagnostics.fn_analysis import DetectedSegment
from sublift.diagnostics.short_subtitle import (
    ShortSubtitleMetrics,
    compute_short_subtitle_metrics,
    format_short_subtitle_report,
)


def _gt(index: int, start: int, end: int, text: str = "") -> SrtEntry:
    return SrtEntry(index=index, start_ms=start, end_ms=end, text=text)


def _det(start: int, end: int) -> DetectedSegment:
    return DetectedSegment(start_ms=start, end_ms=end)


class TestShortSubtitleMetrics:
    """compute_short_subtitle_metrics 行为。"""

    def test_no_short_subtitles(self) -> None:
        """全部字幕时长 >= 阈值，short_gt_count=0。"""
        gt = [_gt(1, 0, 2000), _gt(2, 3000, 5000)]
        detected = [_det(0, 2000), _det(3000, 5000)]
        m = compute_short_subtitle_metrics(detected, gt, threshold_ms=1500)
        assert m.short_gt_count == 0
        assert m.short_matched == 0
        assert m.short_recall == 0.0

    def test_all_short_matched(self) -> None:
        """短字幕全部命中。"""
        gt = [_gt(1, 0, 1000), _gt(2, 2000, 3000)]
        detected = [_det(0, 1000), _det(2000, 3000)]
        m = compute_short_subtitle_metrics(detected, gt, threshold_ms=1500)
        assert m.short_gt_count == 2
        assert m.short_matched == 2
        assert m.short_recall == 1.0

    def test_all_short_missed(self) -> None:
        """短字幕全部漏检。"""
        gt = [_gt(1, 0, 1000), _gt(2, 2000, 3000)]
        detected: list[DetectedSegment] = []
        m = compute_short_subtitle_metrics(detected, gt, threshold_ms=1500)
        assert m.short_gt_count == 2
        assert m.short_matched == 0
        assert m.short_recall == 0.0

    def test_mixed_short_and_long(self) -> None:
        """混合长短字幕，只统计短字幕。"""
        gt = [
            _gt(1, 0, 500),  # 短
            _gt(2, 1000, 4000),  # 长
            _gt(3, 5000, 5800),  # 短
        ]
        detected = [_det(0, 500), _det(1000, 4000)]
        m = compute_short_subtitle_metrics(detected, gt, threshold_ms=1500)
        assert m.short_gt_count == 2
        assert m.short_matched == 1  # gt1 命中，gt3 漏检
        assert m.short_recall == 0.5

    def test_custom_threshold(self) -> None:
        """自定义阈值。"""
        gt = [_gt(1, 0, 1000)]
        detected = [_det(0, 1000)]
        m_default = compute_short_subtitle_metrics(detected, gt, threshold_ms=1500)
        assert m_default.short_recall == 1.0
        m_strict = compute_short_subtitle_metrics(detected, gt, threshold_ms=500)
        assert m_strict.short_gt_count == 0

    def test_threshold_field_recorded(self) -> None:
        gt = [_gt(1, 0, 1000)]
        m = compute_short_subtitle_metrics([], gt, threshold_ms=1200)
        assert m.threshold_ms == 1200

    def test_empty_ground_truth(self) -> None:
        m = compute_short_subtitle_metrics([_det(0, 1000)], [])
        assert m.short_gt_count == 0
        assert m.short_recall == 0.0


class TestReportFormat:
    """format_short_subtitle_report 输出格式。"""

    def test_report_contains_all_configs(self) -> None:
        m1 = ShortSubtitleMetrics(
            short_gt_count=5, short_matched=3, short_recall=0.6, threshold_ms=1500
        )
        m2 = ShortSubtitleMetrics(
            short_gt_count=5, short_matched=5, short_recall=1.0, threshold_ms=1500
        )
        report = format_short_subtitle_report([m1, m2], labels=["baseline", "optimized"])
        assert "baseline" in report
        assert "optimized" in report
        assert "60.0%" in report
        assert "100.0%" in report

    def test_report_default_labels(self) -> None:
        m = ShortSubtitleMetrics(
            short_gt_count=1, short_matched=1, short_recall=1.0, threshold_ms=1500
        )
        report = format_short_subtitle_report([m])
        assert "config_0" in report
