"""字幕导出抽象接口。"""

from __future__ import annotations

from pathlib import Path
from typing import Protocol, runtime_checkable

from sublift.models import SubtitleEntry


@runtime_checkable
class Exporter(Protocol):
    """将字幕条目导出为字幕文件。

    两个方法：
    - format: 纯函数，返回格式化后的字符串。
    - export: 写入文件，内部调用 format。
    """

    def format(self, entries: list[SubtitleEntry]) -> str:
        """将字幕条目格式化为字符串。

        Args:
            entries: 字幕条目列表。

        Returns:
            格式化后的字幕字符串。
        """
        ...

    def export(self, entries: list[SubtitleEntry], output: Path) -> None:
        """导出字幕到文件。

        Args:
            entries: 字幕条目列表。
            output: 输出文件路径。
        """
        ...
