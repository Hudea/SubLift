"""字幕导出（export）测试。

覆盖：
- SrtExporter: 时间码格式化、SRT 格式、文本多行、越界、写文件
- AssExporter/VttExporter: 占位抛 NotImplementedError
- Exporter Protocol: @runtime_checkable isinstance 判定

纯单测，不依赖外部资源。
"""

from __future__ import annotations

from pathlib import Path

import pytest

from sublift.export import AssExporter, Exporter, SrtExporter, VttExporter
from sublift.models import SubtitleEntry


def _entry(start: int, end: int, text: str) -> SubtitleEntry:
    return SubtitleEntry(start_ms=start, end_ms=end, text=text)


class TestFormatTimestamp:
    """SRT 时间码格式化（内部 _format_timestamp）。"""

    def test_zero(self) -> None:
        """0ms -> 00:00:00,000。"""
        assert SrtExporter._format_timestamp(0) == "00:00:00,000"

    def test_milliseconds_only(self) -> None:
        """999ms -> 00:00:00,999。"""
        assert SrtExporter._format_timestamp(999) == "00:00:00,999"

    def test_one_second(self) -> None:
        """1000ms -> 00:00:01,000。"""
        assert SrtExporter._format_timestamp(1000) == "00:00:01,000"

    def test_hms_boundaries(self) -> None:
        """3661500ms -> 01:01:01,500（时分秒各 1）。"""
        assert SrtExporter._format_timestamp(3_661_500) == "01:01:01,500"

    def test_hours_exceed_24(self) -> None:
        """86400000ms (24h) -> 24:00:00,000（SRT 不回绕）。"""
        assert SrtExporter._format_timestamp(86_400_000) == "24:00:00,000"

    def test_negative_raises(self) -> None:
        """负值 -> ValueError（由 _validate 调用链路保证）。"""
        with pytest.raises(ValueError):
            SrtExporter._format_timestamp(-1)


class TestSrtFormat:
    """SrtExporter.format 格式化。"""

    def test_empty_list(self) -> None:
        """空列表 -> 空字符串。"""
        assert SrtExporter().format([]) == ""

    def test_single_entry(self) -> None:
        """单条目 -> 序号 1 + 时间码 + 文本 + 末尾换行。"""
        result = SrtExporter().format([_entry(1000, 2000, "你好")])
        assert result == "1\n00:00:01,000 --> 00:00:02,000\n你好\n"

    def test_multiple_entries_separated_by_blank_line(self) -> None:
        """多条目 -> 序号递增 + 空行分隔。"""
        result = SrtExporter().format(
            [
                _entry(1000, 2000, "你好"),
                _entry(3000, 4000, "再见"),
            ]
        )
        assert result == (
            "1\n00:00:01,000 --> 00:00:02,000\n你好\n\n2\n00:00:03,000 --> 00:00:04,000\n再见\n"
        )

    def test_index_starts_at_1(self) -> None:
        """序号从 1 开始。"""
        result = SrtExporter().format([_entry(0, 1000, "A")])
        assert result.startswith("1\n")

    def test_text_with_newline_preserved(self) -> None:
        """text 含 \\n -> 原样多行输出。"""
        result = SrtExporter().format([_entry(0, 1000, "第一行\n第二行")])
        assert "第一行\n第二行" in result

    def test_trailing_newline(self) -> None:
        """末尾保留单个 \\n。"""
        result = SrtExporter().format([_entry(0, 1000, "A")])
        assert result.endswith("\n")
        assert not result.endswith("\n\n")


class TestSrtValidation:
    """SrtExporter.format 越界校验。"""

    def test_negative_start_raises(self) -> None:
        """start_ms < 0 -> ValueError。"""
        with pytest.raises(ValueError, match="时间不能为负"):
            SrtExporter().format([_entry(-1, 1000, "A")])

    def test_negative_end_raises(self) -> None:
        """end_ms < 0 -> ValueError。"""
        with pytest.raises(ValueError, match="时间不能为负"):
            SrtExporter().format([_entry(0, -1, "A")])

    def test_end_before_start_raises(self) -> None:
        """end_ms < start_ms -> ValueError。"""
        with pytest.raises(ValueError, match="end_ms 不能小于 start_ms"):
            SrtExporter().format([_entry(2000, 1000, "A")])

    def test_equal_start_end_ok(self) -> None:
        """end_ms == start_ms -> 合法（零时长段，不报错）。"""
        result = SrtExporter().format([_entry(1000, 1000, "A")])
        assert "00:00:01,000 --> 00:00:01,000" in result


class TestSrtExport:
    """SrtExporter.export 写文件。"""

    def test_export_writes_file(self, tmp_path: Path) -> None:
        """export 写入文件内容与 format 一致。"""
        output = tmp_path / "out.srt"
        SrtExporter().export([_entry(1000, 2000, "你好")], output)
        assert output.read_text(encoding="utf-8") == ("1\n00:00:01,000 --> 00:00:02,000\n你好\n")

    def test_export_empty_list_writes_empty_file(self, tmp_path: Path) -> None:
        """空列表写入空文件。"""
        output = tmp_path / "empty.srt"
        SrtExporter().export([], output)
        assert output.read_text(encoding="utf-8") == ""


class TestPlaceholderExporters:
    """AssExporter / VttExporter 占位。"""

    def test_ass_format_raises(self) -> None:
        """AssExporter.format -> NotImplementedError。"""
        with pytest.raises(NotImplementedError):
            AssExporter().format([])

    def test_ass_export_raises(self, tmp_path: Path) -> None:
        """AssExporter.export -> NotImplementedError。"""
        with pytest.raises(NotImplementedError):
            AssExporter().export([], tmp_path / "a.ass")

    def test_vtt_format_raises(self) -> None:
        """VttExporter.format -> NotImplementedError。"""
        with pytest.raises(NotImplementedError):
            VttExporter().format([])

    def test_vtt_export_raises(self, tmp_path: Path) -> None:
        """VttExporter.export -> NotImplementedError。"""
        with pytest.raises(NotImplementedError):
            VttExporter().export([], tmp_path / "a.vtt")


class TestExporterProtocol:
    """Exporter Protocol @runtime_checkable。"""

    def test_srt_is_exporter(self) -> None:
        """SrtExporter 满足 Exporter Protocol。"""
        assert isinstance(SrtExporter(), Exporter)

    def test_ass_is_exporter(self) -> None:
        """AssExporter 满足 Exporter Protocol。"""
        assert isinstance(AssExporter(), Exporter)

    def test_vtt_is_exporter(self) -> None:
        """VttExporter 满足 Exporter Protocol。"""
        assert isinstance(VttExporter(), Exporter)

    def test_non_exporter_not_recognized(self) -> None:
        """非 Exporter 对象不被识别。"""
        assert not isinstance(42, Exporter)
        assert not isinstance("string", Exporter)
