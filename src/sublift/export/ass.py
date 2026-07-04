"""ASS 字幕导出占位。

ASS/SSA 格式实现留待后续 Phase，当前仅提供接口占位。
"""

from __future__ import annotations

from pathlib import Path

from sublift.models import SubtitleEntry


class AssExporter:
    """ASS 字幕导出器占位。

    实现留待后续 Phase，调用任一方法均抛 NotImplementedError。
    """

    def format(self, entries: list[SubtitleEntry]) -> str:
        """将字幕条目格式化为 ASS 字符串。

        Args:
            entries: 字幕条目列表。

        Raises:
            NotImplementedError: 始终抛出，ASS 导出尚未实现。
        """
        raise NotImplementedError("ASS 导出尚未实现")

    def export(self, entries: list[SubtitleEntry], output: Path) -> None:
        """导出字幕到 ASS 文件。

        Args:
            entries: 字幕条目列表。
            output: 输出文件路径（.ass）。

        Raises:
            NotImplementedError: 始终抛出，ASS 导出尚未实现。
        """
        raise NotImplementedError("ASS 导出尚未实现")
