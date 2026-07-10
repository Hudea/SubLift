"""OCR 行级选择与段内文本共识（feat-034c / 034d）。

纯函数模块：不依赖 Vision / Pipeline 状态，便于单测。
"""

from __future__ import annotations

import re
from collections import Counter
from dataclasses import dataclass

from sublift.models import (
    SCRIPT_AUTO,
    SCRIPT_CJK,
    SCRIPT_LATIN,
    OcrLine,
    SubtitleProfile,
)

# CJK 统一表意 + 扩展 A 等常用区
_CJK_RE = re.compile(
    r"[\u3400-\u4dbf\u4e00-\u9fff\uf900-\ufaff"
    r"\U00020000-\U0002a6df]"
)
_LATIN_RE = re.compile(r"[A-Za-z]")
_WS_RE = re.compile(r"\s+")


@dataclass(frozen=True)
class LineScore:
    """单行相对 profile 的分解分。"""

    total: float
    script: float
    y_band: float
    height: float
    center: float
    confidence: float


def normalize_ocr_text(text: str) -> str:
    """共识用规范化：去首尾空白、压缩内部空白。"""
    return _WS_RE.sub(" ", text.strip())


def cleanup_subtitle_text(text: str, script: str = SCRIPT_CJK) -> str:
    """选行/共识后的轻量清理：去粘连水印英文、统一常见标点。

    不改变语义主体；用于压低 CER 与 text.noise，避免 PHISON/SON 等尾巴。
    """
    t = normalize_ocr_text(text)
    if not t:
        return ""

    if script in (SCRIPT_CJK, SCRIPT_AUTO):
        # 粘在中文后的纯拉丁尾巴：气候墙SON / 入狱）PHISON
        t = re.sub(r"[A-Za-z]{2,}$", "", t)
        # 独立尾随英文词
        t = re.sub(r"\s+[A-Za-z]{2,}(?:\s+[A-Za-z]{2,})*$", "", t)
        # 行首英文水印
        t = re.sub(r"^[A-Za-z]{2,}(?:\s+[A-Za-z]{2,})*\s+", "", t)

    # 省略号 / 引号 与常见 GT 对齐
    t = t.replace("⋯", "…").replace("...", "…")
    t = t.replace("『", "「").replace("』", "」")
    t = t.replace("－", "-").replace("—", "-")
    # 中文语境下单独句点常为省略：事实上.
    if script == SCRIPT_CJK and t.endswith(".") and cjk_ratio(t[:-1]) >= 0.5:
        t = t[:-1] + "…"

    return normalize_ocr_text(t)


def cjk_ratio(text: str) -> float:
    """文本中 CJK 字符占比（相对非空白字符）。"""
    chars = [c for c in text if not c.isspace()]
    if not chars:
        return 0.0
    cjk = sum(1 for c in chars if _CJK_RE.fullmatch(c))
    return cjk / len(chars)


def latin_ratio(text: str) -> float:
    """文本中拉丁字母占比（相对非空白字符）。"""
    chars = [c for c in text if not c.isspace()]
    if not chars:
        return 0.0
    lat = sum(1 for c in chars if _LATIN_RE.fullmatch(c))
    return lat / len(chars)


def script_score(text: str, script: str) -> float:
    """文字系统匹配分 [0, 1]。"""
    if script == SCRIPT_AUTO:
        return 0.5 + 0.5 * max(cjk_ratio(text), latin_ratio(text))
    if script == SCRIPT_CJK:
        # 纯英文噪声接近 0；中英混排字幕仍可 >0.3
        return cjk_ratio(text)
    if script == SCRIPT_LATIN:
        return latin_ratio(text)
    return 0.0


def score_line(line: OcrLine, profile: SubtitleProfile) -> LineScore:
    """对单行相对 profile 打分。

    权重侧重 script + Y 带（解决新闻横幅/姓名噪声），字号与中心次之。
    """
    s_script = script_score(line.text, profile.script)

    cy = line.box.y + line.box.height / 2.0
    band_lo = float(profile.y_min)
    band_hi = float(profile.y_max) if profile.y_max > profile.y_min else band_lo + 1.0
    band_h = max(1.0, band_hi - band_lo)
    if band_lo <= cy <= band_hi:
        # 带内：越靠近 center_y 越高
        dist = abs(cy - profile.center_y) / band_h
        s_y = max(0.0, 1.0 - dist)
    else:
        # 带外：按带高度归一化惩罚
        overflow = (
            (band_lo - cy) / band_h if cy < band_lo else (cy - band_hi) / band_h
        )
        s_y = max(0.0, 1.0 - overflow)

    # 字号：相对 profile.height
    ph = max(1.0, float(profile.height))
    lh = max(1.0, float(line.box.height))
    ratio = min(lh, ph) / max(lh, ph)
    s_h = ratio

    # 水平中心
    cx = line.box.x + line.box.width / 2.0
    # 用 profile 宽度近似：2*max(center_x, 某种 span)；无 width 时用 |cx-center| / max(center*2,1)
    span = max(float(profile.center_x * 2), 1.0)
    s_c = max(0.0, 1.0 - abs(cx - profile.center_x) / span)

    conf = max(0.0, min(1.0, line.confidence))

    total = (
        0.40 * s_script
        + 0.30 * s_y
        + 0.15 * s_h
        + 0.10 * s_c
        + 0.05 * conf
    )
    return LineScore(
        total=total,
        script=s_script,
        y_band=s_y,
        height=s_h,
        center=s_c,
        confidence=conf,
    )


def select_line(
    lines: tuple[OcrLine, ...] | list[OcrLine],
    profile: SubtitleProfile,
    *,
    min_score: float = 0.28,
    min_script: float = 0.12,
) -> OcrLine | None:
    """从多行中选最佳字幕行。

    Args:
        lines: OCR 行。
        profile: 字幕轨画像。
        min_score: 总分下限；过低视为无可靠字幕行。
        min_script: 目标文字系统下限（抑制纯背景外文）。

    Returns:
        最佳行；无合格行时 None。
    """
    if not lines:
        return None

    best: OcrLine | None = None
    best_score = -1.0
    for line in lines:
        if not line.text.strip():
            continue
        sc = score_line(line, profile)
        if sc.total < min_score:
            continue
        if profile.script != SCRIPT_AUTO and sc.script < min_script:
            continue
        if sc.total > best_score:
            best_score = sc.total
            best = line
    return best


def edit_distance(a: str, b: str) -> int:
    """经典 Levenshtein 距离。"""
    if a == b:
        return 0
    if not a:
        return len(b)
    if not b:
        return len(a)
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, start=1):
        cur = [i]
        for j, cb in enumerate(b, start=1):
            ins = cur[j - 1] + 1
            delete = prev[j] + 1
            sub = prev[j - 1] + (0 if ca == cb else 1)
            cur.append(min(ins, delete, sub))
        prev = cur
    return prev[-1]


def consensus_text(
    samples: list[tuple[str, float]],
    *,
    min_votes: int = 1,
) -> tuple[str, float]:
    """多帧选中文本共识。

    1. 规范化后计票，得票最多者胜（多数）
    2. 平票时用总编辑距离最小的 medoid
    3. confidence 取支持共识样本的均值

    Args:
        samples: ``(text, confidence)`` 列表（已做行级选择后的结果）。
        min_votes: 最少支持票；不足时仍返回最优文本，由调用方决定是否放行。

    Returns:
        ``(text, confidence)``；无样本时 ``("", 0.0)``。
    """
    cleaned: list[tuple[str, float]] = []
    for text, conf in samples:
        norm = normalize_ocr_text(text)
        if not norm:
            continue
        cleaned.append((norm, conf))
    if not cleaned:
        return "", 0.0

    counts = Counter(t for t, _ in cleaned)
    # 多数：按票数，同票按首次出现顺序稳定
    ranked = sorted(counts.items(), key=lambda kv: (-kv[1], kv[0]))
    top_text, top_votes = ranked[0]

    if len(ranked) > 1 and ranked[0][1] == ranked[1][1]:
        # 平票 → medoid
        candidates = [t for t, c in ranked if c == top_votes]
        best_t = candidates[0]
        best_cost = None
        for cand in candidates:
            cost = sum(edit_distance(cand, other) for other, _ in cleaned)
            if best_cost is None or cost < best_cost:
                best_cost = cost
                best_t = cand
        top_text = best_t
        top_votes = counts[top_text]

    if top_votes < min_votes and len(cleaned) >= min_votes:
        # 仍返回，调用方用 conf 策略判断
        pass

    support_confs = [c for t, c in cleaned if t == top_text]
    conf = sum(support_confs) / len(support_confs) if support_confs else 0.0
    return top_text, conf


def should_accept_text(
    text: str,
    confidence: float,
    *,
    profile: SubtitleProfile,
    confidence_threshold: float,
    low_conf_threshold: float,
    support_votes: int,
    min_stable_votes: int = 2,
) -> bool:
    """是否接受最终文本（低置信稳定放行 / 高 conf 仍拒噪声）。

    规则：
    - 空文本：不接受（调用方可仍保留空 entry）
    - conf >= confidence_threshold 且 script 及格：接受
    - conf >= low_conf_threshold 且 script 较好且 support_votes >= min_stable_votes：接受
    - 单帧低 conf：拒绝（避免背景闪一下）
    """
    if not text.strip():
        return False

    s = script_score(text, profile.script)
    # 目标语明显不符：即使高 conf 也不要（整段英文噪声）
    if profile.script == SCRIPT_CJK and s < 0.08 and latin_ratio(text) > 0.5:
        return False
    if profile.script == SCRIPT_LATIN and s < 0.08 and cjk_ratio(text) > 0.5:
        return False

    if confidence >= confidence_threshold and (
        profile.script == SCRIPT_AUTO or s >= 0.08
    ):
        return True

    if (
        confidence >= low_conf_threshold
        and support_votes >= min_stable_votes
        and s >= 0.20
    ):
        return True

    # 单帧中等置信 + 强目标语：介于 low_conf 与 threshold 之间
    if (
        confidence >= max(low_conf_threshold, 0.35)
        and s >= 0.45
        and support_votes >= 1
    ):
        return True

    # 单帧但 conf 够高且 script 很好（强中文）
    return confidence >= confidence_threshold and s >= 0.35
