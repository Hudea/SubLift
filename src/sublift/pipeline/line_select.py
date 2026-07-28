"""OCR 行级选择与段内文本共识（feat-034c / 034d）。

纯函数模块：不依赖 Vision / Pipeline 状态，便于单测。
"""

from __future__ import annotations

import re
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
_CJK_EDGE_CLASS = r"\u3400-\u4dbf\u4e00-\u9fff\uf900-\ufaff"
_LEADING_ATTACHED_LATIN_RE = re.compile(rf"^[A-Za-z0-9._/\-]+(?=[{_CJK_EDGE_CLASS}（【《「『“])")
_TRAILING_ATTACHED_LATIN_RE = re.compile(
    rf"(?<=[{_CJK_EDGE_CLASS}）】》」』”])"
    r"[A-Za-z][A-Za-z0-9 .:_/\-]*[、，。！？…]*$"
)


@dataclass(frozen=True)
class LineScore:
    """单行相对 profile 的分解分。"""

    total: float
    script: float
    y_band: float
    height: float
    center: float
    confidence: float


@dataclass(frozen=True)
class ConsensusResult:
    """段内文本共识结果。"""

    text: str
    confidence: float
    support_votes: int


def normalize_ocr_text(text: str) -> str:
    """共识用规范化：去首尾空白、压缩内部空白。"""
    return _WS_RE.sub(" ", text.strip())


def cleanup_subtitle_text(text: str, script: str = SCRIPT_CJK) -> str:
    """选行/共识后的轻量清理：CJK 粘连横幅边缘与常见标点。

    仅在显式 CJK 模式清理直接粘连边缘；不删除纯英文或常规中英混排。
    """
    t = normalize_ocr_text(text)
    if not t:
        return ""

    if script == SCRIPT_CJK and cjk_ratio(t) > 0.0:
        # Vision 偶尔把横幅与字幕合成一行。仅清理直接粘在 CJK 边界上的
        # 拉丁前后缀；空格分隔的合法混排与夹在中文内部的 ZPD 均保留。
        t = _LEADING_ATTACHED_LATIN_RE.sub("", t, count=1)
        t = _TRAILING_ATTACHED_LATIN_RE.sub("", t, count=1)

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
        overflow = (band_lo - cy) / band_h if cy < band_lo else (cy - band_hi) / band_h
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

    total = 0.40 * s_script + 0.30 * s_y + 0.15 * s_h + 0.10 * s_c + 0.05 * conf
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


def is_similar(a: str, b: str) -> bool:
    """按归一化编辑距离判断两个共识 key 是否属于同一变体簇。"""
    if not a or not b:
        return a == b
    if (a in b or b in a) and min(len(a), len(b)) >= 4:
        return True
    ed = edit_distance(a, b)
    threshold = 0.34 if max(len(a), len(b)) <= 4 else 0.4
    return ed / max(len(a), len(b)) <= threshold


def _consensus_key(text: str, script: str) -> str:
    """生成聚类 key；CJK 画像忽略易变的拉丁横幅，但不修改输出。"""
    normalized = normalize_ocr_text(text)
    if script != SCRIPT_CJK:
        return normalized
    cjk_chars = "".join(char for char in normalized if _CJK_RE.fullmatch(char))
    return cjk_chars or normalized


_LEADING_LATIN_EDGE_RE = re.compile(r"^[A-Za-z][A-Za-z0-9 .:_/\-]*")
_TRAILING_LATIN_EDGE_RE = re.compile(r"[A-Za-z][A-Za-z0-9 .:_/\-]*$")


def _edge_match(text: str, pattern: re.Pattern[str]) -> str:
    match = pattern.search(text)
    return match.group(0).strip() if match else ""


def _trim_unstable_latin_edges(text: str, cluster_texts: list[str]) -> str:
    """仅删除簇内不稳定的拉丁前后缀，保留稳定英文和中英混排主体。"""
    result = text
    leading = _edge_match(result, _LEADING_LATIN_EDGE_RE)
    if leading:
        leading_variants = {
            _edge_match(candidate, _LEADING_LATIN_EDGE_RE) for candidate in cluster_texts
        }
        if len(leading_variants) > 1:
            result = _LEADING_LATIN_EDGE_RE.sub("", result, count=1).strip()

    trailing = _edge_match(result, _TRAILING_LATIN_EDGE_RE)
    if trailing:
        trailing_variants = {
            _edge_match(candidate, _TRAILING_LATIN_EDGE_RE) for candidate in cluster_texts
        }
        if len(trailing_variants) > 1:
            result = _TRAILING_LATIN_EDGE_RE.sub("", result, count=1).strip()
    return result


def consensus_text(
    samples: list[tuple[str, float]],
    *,
    script: str = SCRIPT_AUTO,
) -> ConsensusResult:
    """多帧选中文本共识。

    1. 规范化后聚类相似变体（应对背景英文横幅变化）
    2. 簇内计票，选得票最多的簇
    3. 簇内选与其他样本编辑距离最小的 medoid
    4. confidence 取支持该簇的样本均值

    Args:
        samples: ``(text, confidence)`` 列表（已做行级选择后的结果）。
        script: 目标文字系统。CJK 模式只在聚类 key 中忽略拉丁字符。

    Returns:
        文本、簇平均置信度和真实簇票数。
    """
    cleaned: list[tuple[int, str, str, float]] = []
    for index, (text, conf) in enumerate(samples):
        norm = normalize_ocr_text(text)
        if not norm:
            continue
        cleaned.append((index, norm, _consensus_key(norm, script), conf))
    if not cleaned:
        return ConsensusResult("", 0.0, 0)

    # 以每个样本为中心建立相似邻域，再按票数/总距离/首次出现稳定选簇。
    neighborhoods: list[list[tuple[int, str, str, float]]] = []
    for _index, _text, center_key, _confidence in cleaned:
        neighborhood = [sample for sample in cleaned if is_similar(sample[2], center_key)]
        neighborhoods.append(neighborhood)

    def _cluster_rank(
        cluster: list[tuple[int, str, str, float]],
    ) -> tuple[int, int, int]:
        total_cost = sum(edit_distance(left[2], right[2]) for left in cluster for right in cluster)
        return (-len(cluster), total_cost, min(item[0] for item in cluster))

    best_cluster = min(neighborhoods, key=_cluster_rank)
    votes = len(best_cluster)

    medoid = min(
        best_cluster,
        key=lambda candidate: (
            sum(edit_distance(candidate[2], other[2]) for other in best_cluster),
            sum(edit_distance(candidate[1], other[1]) for other in best_cluster),
            candidate[0],
        ),
    )
    text = medoid[1]
    cluster_texts = [item[1] for item in best_cluster]
    if script == SCRIPT_CJK and len(set(cluster_texts)) > 1:
        text = _trim_unstable_latin_edges(text, cluster_texts)

    confidence = sum(item[3] for item in best_cluster) / votes
    return ConsensusResult(text, confidence, votes)


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

    if confidence >= confidence_threshold and (profile.script == SCRIPT_AUTO or s >= 0.08):
        return True

    if confidence >= low_conf_threshold and support_votes >= min_stable_votes and s >= 0.20:
        return True

    # 单帧中等置信 + 强目标语：介于 low_conf 与 threshold 之间
    if confidence >= max(low_conf_threshold, 0.35) and s >= 0.45 and support_votes >= 1:
        return True

    # 单帧但 conf 够高且 script 很好（强中文）
    return confidence >= confidence_threshold and s >= 0.35
