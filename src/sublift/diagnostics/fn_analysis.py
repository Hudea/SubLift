"""FN（漏检）归因分析。

对 pipeline 产出的检测段与 ground truth 做时间对齐，将每条漏检的真实条
目归类为可解释的 FN 类型，用于定位算法缺陷。

FN 类型：
- ``no_overlap``: 真实条目与任何检测段无时间重叠（完全漏检）。
- ``merged_into_neighbor``: 真实条目与某检测段有重叠，但该检测段同时
  覆盖 ≥2 条真实条目（被合并到相邻段中，dHash 漏检 CHANGE 的典型表现）。
- ``boundary_miss``: 真实条目与检测段有重叠，但重叠未达命中阈值（时
  间边界打偏，无合并关系）。
"""

from __future__ import annotations

from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Protocol, runtime_checkable


@runtime_checkable
class GtEntry(Protocol):
    """ground truth 条目协议（与 benchmark.srt_loader.SrtEntry 结构兼容）。

    使用 Protocol 避免反向依赖 benchmark 包，保持 src/sublift 自包含。
    成员声明为 ``@property`` 以兼容 frozen dataclass 的只读属性。
    """

    @property
    def index(self) -> int: ...

    @property
    def start_ms(self) -> int: ...

    @property
    def end_ms(self) -> int: ...

    @property
    def text(self) -> str: ...


@dataclass(frozen=True)
class DetectedSegment:
    """检测段（与 benchmark 对齐前的简化表示）。

    Attributes:
        start_ms: 段起始时间戳（毫秒）。
        end_ms: 段结束时间戳（毫秒）。
        text: OCR 文本（可为空，归因不依赖此字段）。
    """

    start_ms: int
    end_ms: int
    text: str = ""


@dataclass(frozen=True)
class FnClassification:
    """单条 FN 的归因结果。

    Attributes:
        gt_entry: 漏检的真实条目。
        fn_type: FN 类型（``no_overlap`` / ``merged_into_neighbor`` /
            ``boundary_miss``）。
        related_detected: 与该真实条目重叠最大的检测段（无重叠时为 None）。
        overlap_ms: 与 ``related_detected`` 的重叠毫秒数。
        gt_neighbors_in_same_detected: 当 ``fn_type=merged_into_neighbor`` 时，
            同一检测段覆盖的其他真实条目数量（含本条）。
    """

    gt_entry: GtEntry
    fn_type: str
    related_detected: DetectedSegment | None
    overlap_ms: int
    gt_neighbors_in_same_detected: int = 0


def classify_fn(
    detected: list[DetectedSegment],
    ground_truth: Sequence[GtEntry],
    *,
    match_threshold: float = 0.5,
) -> list[FnClassification]:
    """对每条漏检的真实条目做 FN 归因。

    判定漏检的条件：真实条目未被任何检测段命中（``overlap < max(det_dur,
    gt_dur) * threshold``）。

    Args:
        detected: pipeline 产出的检测段列表。
        ground_truth: ground truth 真实条目列表（满足 ``GtEntry`` 协议）。
        match_threshold: 命中阈值（与 benchmark.alignment 一致，默认 0.5）。

    Returns:
        每条漏检条目对应一个 ``FnClassification``，按 ground truth 顺序排列。
    """
    if not ground_truth:
        return []

    matched: list[bool] = [False] * len(ground_truth)
    best_overlap: list[tuple[int, int]] = [(0, -1)] * len(ground_truth)
    neighbors: list[set[int]] = [set() for _ in range(len(detected))]

    for gt_idx, gt in enumerate(ground_truth):
        best_ov = 0
        best_det_idx = -1
        for det_idx, det in enumerate(detected):
            ov = _overlap_ms(gt.start_ms, gt.end_ms, det.start_ms, det.end_ms)
            if ov > 0:
                neighbors[det_idx].add(gt_idx)
            if ov > best_ov:
                best_ov = ov
                best_det_idx = det_idx
        best_overlap[gt_idx] = (best_ov, best_det_idx)
        if best_det_idx >= 0:
            det = detected[best_det_idx]
            det_dur = det.end_ms - det.start_ms
            gt_dur = gt.end_ms - gt.start_ms
            threshold_ms = max(det_dur, gt_dur) * match_threshold
            if best_ov >= threshold_ms:
                matched[gt_idx] = True

    results: list[FnClassification] = []
    for gt_idx, gt in enumerate(ground_truth):
        if matched[gt_idx]:
            continue
        ov, det_idx = best_overlap[gt_idx]
        if det_idx < 0:
            results.append(
                FnClassification(
                    gt_entry=gt,
                    fn_type="no_overlap",
                    related_detected=None,
                    overlap_ms=0,
                )
            )
            continue

        det = detected[det_idx]
        det_neighbor_count = len(neighbors[det_idx])
        fn_type = "merged_into_neighbor" if det_neighbor_count >= 2 else "boundary_miss"

        results.append(
            FnClassification(
                gt_entry=gt,
                fn_type=fn_type,
                related_detected=det,
                overlap_ms=ov,
                gt_neighbors_in_same_detected=det_neighbor_count,
            )
        )

    return results


def format_fn_report(
    classifications: list[FnClassification],
    output_path: Path | None = None,
) -> str:
    """生成 FN 归因 Markdown 报告。

    Args:
        classifications: ``classify_fn`` 输出的归因列表。
        output_path: 可选，写入文件。

    Returns:
        Markdown 文本。
    """
    from collections import Counter

    lines: list[str] = ["# FN 归因报告", ""]

    counts = Counter(c.fn_type for c in classifications)
    lines.append("## 类型分布")
    lines.append("")
    lines.append("| FN 类型 | 数量 |")
    lines.append("|---|---|")
    for fn_type in ("no_overlap", "merged_into_neighbor", "boundary_miss"):
        lines.append(f"| {fn_type} | {counts.get(fn_type, 0)} |")
    lines.append("")
    lines.append(f"总 FN 数：{len(classifications)}")
    lines.append("")

    lines.append("## 逐条归因")
    lines.append("")
    if not classifications:
        lines.append("- (无漏检)")
    else:
        for c in classifications:
            gt = c.gt_entry
            lines.append(
                f"- #{gt.index} {_ms_to_tc(gt.start_ms)}→{_ms_to_tc(gt.end_ms)}"
                f" `{gt.text}`"
            )
            lines.append(f"  - 类型: `{c.fn_type}`")
            if c.related_detected is not None:
                det = c.related_detected
                lines.append(
                    f"  - 关联检测段: {_ms_to_tc(det.start_ms)}→{_ms_to_tc(det.end_ms)}"
                    f" 重叠 {c.overlap_ms}ms"
                )
            if c.gt_neighbors_in_same_detected > 0:
                lines.append(
                    f"  - 同段真实条目数: {c.gt_neighbors_in_same_detected}"
                    "（合并漏检）"
                )
    lines.append("")

    report = "\n".join(lines)
    if output_path is not None:
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_text(report, encoding="utf-8")
    return report


def _overlap_ms(a_start: int, a_end: int, b_start: int, b_end: int) -> int:
    """两个时间区间的重叠毫秒数（无重叠返回 0）。"""
    return max(0, min(a_end, b_end) - max(a_start, b_start))


def _ms_to_tc(ms: int) -> str:
    """毫秒 → ``HH:MM:SS,mmm`` 时间码（SRT 风格）。"""
    h, rem = divmod(ms, 3_600_000)
    m, rem = divmod(rem, 60_000)
    s, ms_part = divmod(rem, 1_000)
    return f"{h:02d}:{m:02d}:{s:02d},{ms_part:03d}"
