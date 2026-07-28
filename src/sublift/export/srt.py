"""SRT 字幕导出实现。

SRT 格式规范：
    1
    00:00:01,000 --> 00:00:02,000
    你好世界

    2
    00:00:03,000 --> 00:00:04,000
    再见

- 时间码：HH:MM:SS,mmm（逗号分隔毫秒）
- 序号从 1 开始，按列表顺序
- 条目间空行分隔，文件末尾保留换行
- 小时数可超 24（不回绕）
"""

from __future__ import annotations

from pathlib import Path

from sublift.models import SubtitleEntry


class SrtExporter:
    """将字幕条目导出为 SRT 格式。"""

    def format(self, entries: list[SubtitleEntry], *, drop_empty: bool = True) -> str:
        """将字幕条目格式化为 SRT 字符串。

        Args:
            entries: 字幕条目列表。
            drop_empty: 是否自动滤除空文本条目（默认 True）。

        Returns:
            SRT 格式字符串，条目间空行分隔，末尾保留换行；
            空列表或全部为空文本时返回空字符串。

        Raises:
            ValueError: 任一条目 start_ms/end_ms 为负或 end_ms < start_ms。
        """
        if not entries:
            return ""
        if drop_empty:
            entries = [e for e in entries if e.text and e.text.strip()]
        if not entries:
            return ""
        blocks: list[str] = []
        for idx, entry in enumerate(entries, start=1):
            self._validate(entry)
            start_tc = self._format_timestamp(entry.start_ms)
            end_tc = self._format_timestamp(entry.end_ms)
            blocks.append(f"{idx}\n{start_tc} --> {end_tc}\n{entry.text}")
        return "\n\n".join(blocks) + "\n"

    def export(
        self, entries: list[SubtitleEntry], output: Path, *, drop_empty: bool = True
    ) -> None:
        """导出字幕到 SRT 文件。

        Args:
            entries: 字幕条目列表。
            output: 输出文件路径（.srt）。
            drop_empty: 是否自动滤除空文本条目（默认 True）。

        Raises:
            ValueError: 任一条目时间越界（见 format）。
        """
        output.write_text(self.format(entries, drop_empty=drop_empty), encoding="utf-8")

    @staticmethod
    def _validate(entry: SubtitleEntry) -> None:
        """校验条目时间合法性。

        Args:
            entry: 待校验的字幕条目。

        Raises:
            ValueError: start_ms/end_ms 为负或 end_ms < start_ms。
        """
        if entry.start_ms < 0 or entry.end_ms < 0:
            raise ValueError(f"时间不能为负: start_ms={entry.start_ms}, end_ms={entry.end_ms}")
        if entry.end_ms < entry.start_ms:
            raise ValueError(f"end_ms 不能小于 start_ms: {entry.end_ms} < {entry.start_ms}")

    @staticmethod
    def _format_timestamp(ms: int) -> str:
        """将毫秒时间戳格式化为 SRT 时间码 HH:MM:SS,mmm。

        Args:
            ms: 毫秒数，必须 >= 0。

        Returns:
            如 "01:01:01,500"。

        Raises:
            ValueError: ms 为负数。
        """
        if ms < 0:
            raise ValueError(f"时间戳不能为负: {ms}")
        h, rem = divmod(ms, 3_600_000)
        m, rem = divmod(rem, 60_000)
        s, ms_part = divmod(rem, 1_000)
        return f"{h:02d}:{m:02d}:{s:02d},{ms_part:03d}"
