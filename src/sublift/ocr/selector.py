"""字幕行 selector（feat-033c）。

在 ROI 内根据 SubtitleProfile 从 OCR observations 中选择目标字幕行，
过滤背景英文/水印/标牌/伪文字。

纯函数，无 I/O 依赖，可安全在 asyncio.to_thread 中调用。
"""

from __future__ import annotations

from sublift.models import OcrLine, SubtitleProfile


def select_lines(
    lines: list[OcrLine],
    profile: SubtitleProfile,
) -> tuple[str, float]:
    """根据 profile 从 OCR observations 中筛选目标字幕行。

    算法：
    1. y 轨道过滤：line 中心 y 落在 [y_center - y_tolerance, y_center + y_tolerance]
    2. 行高过滤：line 高度 >= line_height * 0.5（排除背景细小文字）
    3. 截断：若超过 max_lines 条，取置信度最高的 max_lines 条
    4. y 排序：按 y 升序（上→下）保证阅读顺序
    5. 拼接：text 用 "\\n" 连接，confidence 取选中行均值

    Args:
        lines: OCR per-line observations（crop-relative bbox）。
        profile: 目标字幕层约束。

    Returns:
        (拼接文本, 置信度均值)。无匹配行返回 ("", 0.0)。
    """
    candidates = _filter_candidates(lines, profile)
    if not candidates:
        return "", 0.0
    return _compose(candidates, profile.max_lines)


def _filter_candidates(
    lines: list[OcrLine],
    profile: SubtitleProfile,
) -> list[OcrLine]:
    """y 轨道 + 行高过滤（feat-033c / feat-034c 复用）。

    返回通过 y 轨道和行高过滤的候选行（未截断、未排序）。
    persistent filter 复用此函数在剔除持久背景行后重新筛选。
    """
    candidates = [
        ln
        for ln in lines
        if abs(_center_y(ln) - profile.y_center) <= profile.y_tolerance
    ]
    if not candidates:
        return []
    candidates = [
        ln for ln in candidates if ln.bbox.height >= profile.line_height * 0.5
    ]
    return candidates


def _compose(candidates: list[OcrLine], max_lines: int) -> tuple[str, float]:
    """截断 + y 排序 + 拼接（feat-034c 从 select_lines 抽出复用）。"""
    if not candidates:
        return "", 0.0
    if len(candidates) > max_lines:
        candidates = sorted(candidates, key=lambda ln: -ln.confidence)[:max_lines]
    candidates.sort(key=lambda ln: ln.bbox.y)
    text = "\n".join(ln.text for ln in candidates)
    confidence = sum(ln.confidence for ln in candidates) / len(candidates)
    return text, confidence


def _center_y(line: OcrLine) -> float:
    """line bbox 的垂直中心 y。"""
    return line.bbox.y + line.bbox.height / 2.0
