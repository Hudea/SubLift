"""去重合并（F6）。

清理 timeline + OCR 产出的带文本时间段：
- 合并连续相同文本段（归一化后相等 + gap ≤ merge_gap_ms）
- 过滤过短噪声段（duration < min_duration_ms）
- 消除 OCR 抖动（短误识段被过滤后，两侧相同段变相邻再合并）

3-pass 算法（顺序关键）：
  Pass 1 - 合并：相邻相同 + gap 合法 → 合并（两个短相同段合并后可能变合法）
  Pass 2 - 过滤：duration < min_duration_ms → 丢弃
  Pass 3 - 合并：Pass 2 删段后原本被隔开的相同段变相邻 → 再合并一次
"""

from __future__ import annotations

from sublift.models import SubtitleEntry


def merge_entries(
    entries: list[SubtitleEntry],
    merge_gap_ms: int = 1000,
    min_duration_ms: int = 500,
) -> list[SubtitleEntry]:
    """去重合并字幕条目。

    Args:
        entries: 已按时间排序的字幕条目列表。
        merge_gap_ms: 合并间隔阈值（毫秒），相邻段 gap <= 此值才合并。
        min_duration_ms: 最小持续时间阈值（毫秒），duration < 此值的段被丢弃。

    Returns:
        清理后的字幕条目列表（新列表，不改变输入）。
    """
    if not entries:
        return []

    merged = _merge_adjacent(entries, merge_gap_ms)
    filtered = _filter_short(merged, min_duration_ms)
    return _merge_adjacent(filtered, merge_gap_ms)


def _merge_adjacent(
    entries: list[SubtitleEntry],
    merge_gap_ms: int,
) -> list[SubtitleEntry]:
    """合并相邻相同文本段（归一化后相等 + gap <= merge_gap_ms）。

    合并后保留第一段的文本，start 取首段 start，end 取末段 end。
    """
    if not entries:
        return []

    result: list[SubtitleEntry] = [entries[0]]

    for current in entries[1:]:
        last = result[-1]
        gap = current.start_ms - last.end_ms

        if gap <= merge_gap_ms and _normalize(current.text) == _normalize(last.text):
            result[-1] = SubtitleEntry(
                start_ms=last.start_ms,
                end_ms=current.end_ms,
                text=last.text,
            )
        else:
            result.append(current)

    return result


def _filter_short(
    entries: list[SubtitleEntry],
    min_duration_ms: int,
) -> list[SubtitleEntry]:
    """过滤 duration < min_duration_ms 的段。"""
    return [e for e in entries if e.end_ms - e.start_ms >= min_duration_ms]


def _normalize(text: str) -> str:
    """归一化文本用于比较（不改变输出文本）。

    移除所有空白字符。中文字幕中空白通常是 OCR 噪声，移除后比较更稳健；
    英文场景若需保留词间空格可在后续扩展。
    """
    return "".join(text.split())
