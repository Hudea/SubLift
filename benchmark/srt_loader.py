"""SRT 字幕文件加载与解析。

ground truth 与 pipeline 输出都通过 ``load_srt`` 加载为 ``SrtEntry`` 列表，
时间统一为毫秒。解析器对 BOM / CRLF 容错，跳过空行与序号行。
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

_TIME_RE = re.compile(
    r"(\d{2}):(\d{2}):(\d{2})[,.](\d{3})\s*-->\s*(\d{2}):(\d{2}):(\d{2})[,.](\d{3})"
)


@dataclass(frozen=True)
class SrtEntry:
    """一条字幕条目，时间以毫秒表示。"""

    index: int
    start_ms: int
    end_ms: int
    text: str


class SrtParseError(ValueError):
    """SRT 解析失败。"""


def _srt_time_to_ms(h: str, m: str, s: str, ms: str) -> int:
    return int(h) * 3_600_000 + int(m) * 60_000 + int(s) * 1_000 + int(ms)


def load_srt(path: Path) -> list[SrtEntry]:
    """从 SRT 文件加载字幕条目。

    Args:
        path: ``.srt`` 文件路径。

    Returns:
        按出现顺序排列的 ``SrtEntry`` 列表，序号从 1 起递增。

    Raises:
        SrtParseError: 文件中无有效时间码行。
        FileNotFoundError: 文件不存在。
    """
    text = path.read_text(encoding="utf-8-sig")
    return parse_srt_text(text)


def parse_srt_text(text: str) -> list[SrtEntry]:
    """从 SRT 文本字符串解析字幕条目。

    与 ``load_srt`` 共享解析逻辑；用于 in-memory 测试与 pipeline 输出直接比对。

    Args:
        text: SRT 文件内容（允许 CRLF / 前导 BOM）。

    Returns:
        ``SrtEntry`` 列表。

    Raises:
        SrtParseError: 未找到任何时间码行。
    """
    entries: list[SrtEntry] = []
    current_text_lines: list[str] = []
    start_ms = 0
    end_ms = 0
    saw_timecode = False
    index = 0

    for raw_line in text.splitlines():
        line = raw_line.strip()
        m = _TIME_RE.match(line)
        if m:
            # 上一条 cue 即使文本为空也保留（feat-033 空轴 / 评测 timing）
            if saw_timecode:
                index += 1
                entries.append(SrtEntry(index, start_ms, end_ms, _join_text(current_text_lines)))
                current_text_lines = []
            saw_timecode = True
            start_ms = _srt_time_to_ms(m[1], m[2], m[3], m[4])
            end_ms = _srt_time_to_ms(m[5], m[6], m[7], m[8])
        elif line and not line.isdigit():
            current_text_lines.append(line)

    if saw_timecode:
        index += 1
        entries.append(SrtEntry(index, start_ms, end_ms, _join_text(current_text_lines)))

    if not saw_timecode:
        raise SrtParseError("SRT 文本中未找到任何时间码行")

    return entries


def _join_text(lines: list[str]) -> str:
    """将多行字幕文本合并为单行（保留空格分隔的双语字幕结构）。"""
    return "\n".join(lines)
