"""持久背景文字过滤（feat-034c）。

跨段时序统计识别持久背景文字（ticker / 水印），在 Pipeline.finalize
后处理阶段剔除。单帧 selector（feat-033c）对「同带同高、水平重叠」的
ticker 失效：y 轨道过滤失效、行高过滤失效、单条 observation 合并。
本模块通过跨段时序统计解决。

双条件算法（任一命中即判为持久背景）：

- 条件 A（固定水印）：同一文本指纹在同一 y_bin 连续出现段数
  ≥ min_repeat_segments。
- 条件 B（ticker 文本片段多变）：同一 y_bin 累计不同文本指纹数
  ≥ min_distinct_texts，整个 y_bin 判为持久背景带。

纯函数，无 I/O 依赖，可安全在 asyncio.to_thread 中调用。
"""

from __future__ import annotations

import re
from dataclasses import replace

from sublift.models import OcrLine, SubtitleEntry, SubtitleProfile
from sublift.ocr.selector import _center_y, _compose, _filter_candidates

# 指纹由 (归一化文本, y_bin) 组成
_Fingerprint = tuple[str, int]


def filter_persistent_text(
    entries: list[SubtitleEntry],
    segment_lines: list[list[OcrLine]],
    profile: SubtitleProfile,
) -> list[SubtitleEntry]:
    """跨段时序过滤持久背景文字，重写各段 entry.text。

    Args:
        entries: 已闭合段的字幕条目（按时间顺序）。
        segment_lines: 每段的 OcrLine 列表，与 entries 同 index 对齐。
        profile: 目标字幕层约束，含 persistent_text_policy。policy 为 None
            或 enabled=False 时原样返回 entries。

    Returns:
        新的 SubtitleEntry 列表（保留 start_ms/end_ms，重写 text/confidence）。
        若某段剔除持久行后无候选行，text 置空（由后续 dedupe 过滤）。
    """
    policy = profile.persistent_text_policy
    if policy is None or not policy.enabled:
        return list(entries)

    if len(entries) != len(segment_lines):
        # 长度不一致是调用方 bug，保守返回原样
        return list(entries)

    if not entries:
        return []

    # --- 步骤 1：用 selector 选出每段的目标行，记录其 y_bin 作为 target_bins ---
    # 目标行是「相信的那一层文字」，不应被条件 A/B 误伤。
    # ticker 即使在 y_tolerance 内，只要 y_center 与目标行不同（不同 bin），
    # 仍可被条件 B 识别为持久背景。
    target_bins = _collect_target_bins(segment_lines, profile, policy.y_bin_ratio)

    # --- 步骤 2：收集每段的指纹集合（段内去重） ---
    segment_fingerprints = _collect_segment_fingerprints(
        segment_lines, profile, policy.y_bin_ratio
    )

    # --- 步骤 3：条件 A — 非目标轨道上同指纹最长连续出现段数 ≥ K1 ---
    persistent_fps_a = _find_persistent_by_repeat(
        segment_fingerprints, policy.min_repeat_segments, target_bins
    )

    # --- 步骤 4：条件 B — 非目标轨道上同 y_bin 不同指纹数 ≥ M → 整个 y_bin 标记 ---
    persistent_fps_b = _find_persistent_bins_by_distinct(
        segment_fingerprints, policy.min_distinct_texts, target_bins
    )

    persistent_fps = persistent_fps_a | persistent_fps_b

    if not persistent_fps:
        return list(entries)

    # --- 步骤 5：剔除持久行，重新合成每段 text/confidence ---
    result: list[SubtitleEntry] = []
    for i, entry in enumerate(entries):
        lines = segment_lines[i]
        kept = [
            ln
            for ln in lines
            if _fingerprint(ln, profile, policy.y_bin_ratio) not in persistent_fps
        ]
        candidates = _filter_candidates(kept, profile)
        text, confidence = _compose(candidates, profile.max_lines)
        result.append(replace(entry, text=text, confidence=confidence))
    return result


def _collect_target_bins(
    segment_lines: list[list[OcrLine]],
    profile: SubtitleProfile,
    y_bin_ratio: float,
) -> set[int]:
    """收集所有段中被 selector 选为目标行的 y_bin 集合。

    先对每段跑 _filter_candidates（与 select_lines 同过滤逻辑），选出通过 y 轨道
    + 行高过滤的候选行；这些行的 y_bin 即目标轨道。条件 A/B 在这些 bin 上不判定，
    避免把目标字幕（无论单行/双行、文本多变或重复）误判为持久背景。
    """
    bin_size = max(1.0, profile.line_height * y_bin_ratio)
    target_bins: set[int] = set()
    for lines in segment_lines:
        candidates = _filter_candidates(lines, profile)
        for ln in candidates:
            target_bins.add(int(_center_y(ln) / bin_size))
    return target_bins


def _collect_segment_fingerprints(
    segment_lines: list[list[OcrLine]],
    profile: SubtitleProfile,
    y_bin_ratio: float,
) -> list[set[_Fingerprint]]:
    """收集每段出现的指纹集合（段内去重）。

    Returns:
        长度 == len(segment_lines) 的列表，每项是该段出现过的指纹 set。
    """
    result: list[set[_Fingerprint]] = []
    for lines in segment_lines:
        fps: set[_Fingerprint] = set()
        for ln in lines:
            fps.add(_fingerprint(ln, profile, y_bin_ratio))
        result.append(fps)
    return result


def _find_persistent_by_repeat(
    segment_fingerprints: list[set[_Fingerprint]],
    min_repeat_segments: int,
    target_bins: set[int],
) -> set[_Fingerprint]:
    """条件 A：同指纹最长连续出现段数 ≥ min_repeat_segments。

    仅对非目标 y 轨道判定：target_bins 上的指纹被排除，避免把
    「目标字幕本身在多段重复同一句」误判为水印。

    遍历段序列，对每个指纹维护「当前连续计数」：指纹出现则 +1，否则归零。
    记录每个指纹的最大连续计数，≥ 阈值则标记为持久。

    Returns:
        被标记为持久的指纹集合（含 y_bin 信息）。
    """
    current_streak: dict[_Fingerprint, int] = {}
    max_streak: dict[_Fingerprint, int] = {}

    for fps in segment_fingerprints:
        # 排除目标 bin 上的指纹
        non_target_fps = {fp for fp in fps if fp[1] not in target_bins}
        new_current: dict[_Fingerprint, int] = {}
        for fp in non_target_fps:
            prev = current_streak.get(fp, 0)
            streak = prev + 1
            new_current[fp] = streak
            max_streak[fp] = max(max_streak.get(fp, 0), streak)
        current_streak = new_current

    return {fp for fp, streak in max_streak.items() if streak >= min_repeat_segments}


def _find_persistent_bins_by_distinct(
    segment_fingerprints: list[set[_Fingerprint]],
    min_distinct_texts: int,
    target_bins: set[int],
) -> set[_Fingerprint]:
    """条件 B：同 y_bin 累计不同指纹数 ≥ min_distinct_texts → 整个 y_bin 标记。

    仅对非目标 y 轨道判定：target_bins（profile.y_center 所在 bin）被排除，
    避免把「目标字幕本身文本多变」误判为持久背景。

    Returns:
        被标记为持久的指纹集合（含 y_bin 信息）。
    """
    bin_texts: dict[int, set[str]] = {}
    for fps in segment_fingerprints:
        for fp_text, fp_bin in fps:
            if fp_bin in target_bins:
                continue
            bin_texts.setdefault(fp_bin, set()).add(fp_text)

    persistent_bins = {
        bin_id
        for bin_id, texts in bin_texts.items()
        if len(texts) >= min_distinct_texts
    }
    if not persistent_bins:
        return set()

    result: set[_Fingerprint] = set()
    for fps in segment_fingerprints:
        for fp in fps:
            if fp[1] in persistent_bins:
                result.add(fp)
    return result


def _fingerprint(
    line: OcrLine,
    profile: SubtitleProfile,
    y_bin_ratio: float,
) -> _Fingerprint:
    """计算 OcrLine 的指纹（归一化文本 + y_bin）。

    归一化：去所有空白与标点，转小写。ticker 被 Vision 切成不同片段时，
    归一化后若文本相同则归为同一指纹；若文本不同则靠条件 B 的 y_bin 兜底。
    """
    norm_text = _normalize_text(line.text)
    bin_size = max(1.0, profile.line_height * y_bin_ratio)
    y_bin = int(_center_y(line) / bin_size)
    return (norm_text, y_bin)


_PUNCT_RE = re.compile(r"[\s\W_]+", re.UNICODE)


def _normalize_text(text: str) -> str:
    """归一化文本：去空白、去标点、转小写。

    用于指纹比较。中英文标点、空格、连字符等都不影响判定。
    """
    return _PUNCT_RE.sub("", text).lower()
