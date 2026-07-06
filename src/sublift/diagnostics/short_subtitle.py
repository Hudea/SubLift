"""短字幕漏检专项指标（feat-031c）。

针对 ``duration < threshold``（默认 1500ms）的短字幕单独统计召回率，评估
``presence_threshold`` / ``hysteresis_frames`` / ``min_duration_ms`` 对
短字幕的影响。

短字幕漏检主要属于 IN/OUT 或采样/迟滞不足，SSIM patrol 只补强 CHANGE，
不作为短字幕主解法。本模块为参数扫描（``scripts/scan_params.py``）提供
量化指标。
"""

from __future__ import annotations

from collections.abc import Sequence
from dataclasses import dataclass

from sublift.diagnostics.fn_analysis import (
    DetectedSegment,
    GtEntry,
    classify_fn,
)


@dataclass(frozen=True)
class ShortSubtitleMetrics:
    """短字幕指标。

    Attributes:
        short_gt_count: ground truth 中 duration < threshold 的条目数。
        short_matched: 其中被检测命中的数量。
        short_recall: short_matched / short_gt_count（无短字幕时为 0）。
        threshold_ms: 短字幕时长阈值（毫秒）。
    """

    short_gt_count: int
    short_matched: int
    short_recall: float
    threshold_ms: int


def compute_short_subtitle_metrics(
    detected: list[DetectedSegment],
    ground_truth: Sequence[GtEntry],
    *,
    threshold_ms: int = 1500,
    match_threshold: float = 0.5,
) -> ShortSubtitleMetrics:
    """计算短字幕召回率。

    Args:
        detected: pipeline 产出的检测段列表。
        ground_truth: ground truth 真实条目列表（满足 ``GtEntry`` 协议）。
        threshold_ms: 短字幕时长阈值（毫秒），默认 1500。
        match_threshold: 命中阈值（与 ``classify_fn`` 一致）。

    Returns:
        ``ShortSubtitleMetrics``。
    """
    short_gt = [
        gt for gt in ground_truth if (gt.end_ms - gt.start_ms) < threshold_ms
    ]
    if not short_gt:
        return ShortSubtitleMetrics(
            short_gt_count=0,
            short_matched=0,
            short_recall=0.0,
            threshold_ms=threshold_ms,
        )

    classifications = classify_fn(
        detected, short_gt, match_threshold=match_threshold
    )
    fn_indices = {c.gt_entry.index for c in classifications}
    short_matched = sum(1 for gt in short_gt if gt.index not in fn_indices)

    return ShortSubtitleMetrics(
        short_gt_count=len(short_gt),
        short_matched=short_matched,
        short_recall=short_matched / len(short_gt),
        threshold_ms=threshold_ms,
    )


def format_short_subtitle_report(
    metrics_list: list[ShortSubtitleMetrics],
    *,
    labels: list[str] | None = None,
) -> str:
    """格式化多组短字幕指标为 Markdown 对比表。

    Args:
        metrics_list: 多次参数扫描的指标列表。
        labels: 可选标签（与 metrics_list 等长）。

    Returns:
        Markdown 文本。
    """
    if labels is None:
        labels = [f"config_{i}" for i in range(len(metrics_list))]

    lines: list[str] = ["# 短字幕参数扫描报告", ""]
    lines.append("| 配置 | 短字幕数 | 命中 | 召回率 | 阈值(ms) |")
    lines.append("|---|---|---|---|---|")
    for label, m in zip(labels, metrics_list, strict=True):
        lines.append(
            f"| {label} | {m.short_gt_count} | {m.short_matched} | "
            f"{m.short_recall * 100:.1f}% | {m.threshold_ms} |"
        )
    lines.append("")
    return "\n".join(lines)
