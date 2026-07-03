"""字幕导出抽象接口。"""

from __future__ import annotations

from pathlib import Path
from typing import Protocol

from sublift.models import SubtitleEntry


class Exporter(Protocol):
    """将字幕条目导出为字幕文件。"""

    def export(self, entries: list[SubtitleEntry], output: Path) -> None:
        """导出字幕到文件。

        Args:
            entries: 字幕条目列表。
            output: 输出文件路径。
        """
        ...
