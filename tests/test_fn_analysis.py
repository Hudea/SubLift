"""FN 归因分析测试。

用合成的检测段与 ground truth 验证三种 FN 类型分类。
"""

from __future__ import annotations

from pathlib import Path

import pytest

pytest.importorskip("benchmark.srt_loader")
from benchmark.srt_loader import SrtEntry

from sublift.diagnostics.fn_analysis import (
    DetectedSegment,
    classify_fn,
    format_fn_report,
)


def _gt(index: int, start: int, end: int, text: str = "") -> SrtEntry:
    return SrtEntry(index=index, start_ms=start, end_ms=end, text=text)


def _det(start: int, end: int, text: str = "") -> DetectedSegment:
    return DetectedSegment(start_ms=start, end_ms=end, text=text)


class TestNoOverlap:
    """FN 类型：no_overlap（真实条目与任何检测段无重叠）。"""

    def test_pure_no_overlap(self) -> None:
        gt = [_gt(1, 1000, 2000, "你好")]
        detected: list[DetectedSegment] = []
        results = classify_fn(detected, gt)
        assert len(results) == 1
        assert results[0].fn_type == "no_overlap"
        assert results[0].related_detected is None
        assert results[0].overlap_ms == 0

    def test_partial_overlap_zero_when_disjoint(self) -> None:
        gt = [_gt(1, 1000, 2000)]
        detected = [_det(3000, 4000)]
        results = classify_fn(detected, gt)
        assert len(results) == 1
        assert results[0].fn_type == "no_overlap"


class TestMergedIntoNeighbor:
    """FN 类型：merged_into_neighbor（1 个检测段覆盖 ≥2 条真实条目）。"""

    def test_two_gt_in_one_det(self) -> None:
        """2 条短真实条目被 1 个长检测段覆盖，均未达命中阈值。"""
        gt = [
            _gt(1, 1000, 1100, "你好"),
            _gt(2, 1600, 1700, "世界"),
        ]
        detected = [_det(1000, 2000)]
        results = classify_fn(detected, gt)
        assert len(results) == 2
        assert all(r.fn_type == "merged_into_neighbor" for r in results)
        assert all(r.gt_neighbors_in_same_detected == 2 for r in results)
        assert all(r.related_detected is not None for r in results)

    def test_three_gt_in_one_det(self) -> None:
        """3 条短真实条目被 1 个长检测段覆盖。"""
        gt = [
            _gt(1, 1000, 1200, "一"),
            _gt(2, 1400, 1600, "二"),
            _gt(3, 1800, 2000, "三"),
        ]
        detected = [_det(1000, 2200)]
        results = classify_fn(detected, gt)
        assert len(results) == 3
        assert all(r.fn_type == "merged_into_neighbor" for r in results)
        assert all(r.gt_neighbors_in_same_detected == 3 for r in results)

    def test_zootopia_segment33_pattern(self) -> None:
        """复现段 33 模式：4 句字幕被合并为 1 个长检测段。"""
        gt = [
            _gt(1, 106800, 109200, "能不顾他们惊人的差异"),
            _gt(2, 109600, 112800, "彻底化解偏见和刻板印象"),
            _gt(3, 113000, 117600, "那也许我们都能接受彼此的差异"),
            _gt(4, 117800, 120200, "一起成为更好的动物"),
        ]
        detected = [_det(107400, 119000)]
        results = classify_fn(detected, gt)
        assert len(results) == 4
        assert all(r.fn_type == "merged_into_neighbor" for r in results)


class TestBoundaryMiss:
    """FN 类型：boundary_miss（有重叠但未达命中阈值，无合并关系）。"""

    def test_small_overlap_single_gt(self) -> None:
        """1 条真实 + 1 条检测，重叠很小未命中，无合并。"""
        gt = [_gt(1, 1000, 2000, "你好")]
        detected = [_det(1900, 2100)]
        results = classify_fn(detected, gt)
        assert len(results) == 1
        assert results[0].fn_type == "boundary_miss"
        assert results[0].overlap_ms == 100
        assert results[0].gt_neighbors_in_same_detected == 1


class TestMatched:
    """命中条目不产生 FN 归因。"""

    def test_matched_no_fn(self) -> None:
        gt = [_gt(1, 1000, 2000, "你好")]
        detected = [_det(1000, 2000)]
        results = classify_fn(detected, gt)
        assert len(results) == 0

    def test_partial_match_no_fn(self) -> None:
        """重叠达阈值不算 FN。"""
        gt = [_gt(1, 1000, 2000, "你好")]
        detected = [_det(1200, 2200)]
        results = classify_fn(detected, gt)
        assert len(results) == 0

    def test_mixed_matched_and_fn(self) -> None:
        gt = [
            _gt(1, 1000, 2000, "命中"),
            _gt(2, 3000, 4000, "漏检"),
        ]
        detected = [_det(1000, 2000)]
        results = classify_fn(detected, gt)
        assert len(results) == 1
        assert results[0].gt_entry.index == 2


class TestEdgeCases:
    """边界情况。"""

    def test_empty_ground_truth(self) -> None:
        results = classify_fn([_det(0, 1000)], [])
        assert results == []

    def test_empty_detected(self) -> None:
        gt = [_gt(1, 0, 1000)]
        results = classify_fn([], gt)
        assert len(results) == 1
        assert results[0].fn_type == "no_overlap"

    def test_custom_threshold(self) -> None:
        """提高阈值使原本命中的变 FN。"""
        gt = [_gt(1, 1000, 2000)]
        detected = [_det(1500, 2500)]
        results_default = classify_fn(detected, gt, match_threshold=0.5)
        assert len(results_default) == 0
        results_strict = classify_fn(detected, gt, match_threshold=0.9)
        assert len(results_strict) == 1


class TestReportFormat:
    """format_fn_report 输出格式。"""

    def test_report_contains_all_types(self) -> None:
        """构造同时包含三种 FN 类型的场景。"""
        gt = [
            _gt(1, 1000, 1100, "合并1"),  # 与 gt2 一起被 det1 合并
            _gt(2, 1200, 1300, "合并2"),
            _gt(3, 3000, 4000, "无重叠"),  # no_overlap
            _gt(4, 5000, 6000, "边界"),  # boundary_miss
        ]
        detected = [
            _det(1000, 1400),  # 覆盖 gt1+gt2，但重叠均未达阈值
            _det(5100, 5300),  # 与 gt4 重叠 200ms，未达 max(1000,200)*0.5=500
        ]
        results = classify_fn(detected, gt)
        report = format_fn_report(results)
        assert "merged_into_neighbor" in report
        assert "no_overlap" in report
        assert "boundary_miss" in report
        assert "总 FN 数：4" in report

    def test_report_written_to_file(self, tmp_path: Path) -> None:
        gt = [_gt(1, 1000, 2000, "漏检")]
        results = classify_fn([], gt)
        out = tmp_path / "fn.md"
        format_fn_report(results, output_path=out)
        assert out.exists()
        content = out.read_text(encoding="utf-8")
        assert "漏检" in content

    def test_report_empty_fn(self) -> None:
        report = format_fn_report([])
        assert "总 FN 数：0" in report
        assert "(无漏检)" in report
