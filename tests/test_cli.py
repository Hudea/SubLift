"""CLI 入口测试。

产品 extract 已关闭为 Native-only fail-closed：Python 包不再转发 Worker，
也不接受 --runtime / SUBLIFT_RUNTIME。
"""

from __future__ import annotations

from pathlib import Path

import pytest

from sublift.cli import build_parser, main


class TestParserDefaults:
    """参数解析默认值。"""

    def test_extract_defaults(self) -> None:
        parser = build_parser()
        args = parser.parse_args(["extract", "video.mp4"])
        assert args.command == "extract"
        assert args.video == "video.mp4"
        assert args.output == "output.srt"
        assert args.fps == 5.0
        assert args.confidence == 0.5
        assert args.engine == "vision"
        assert args.script == "auto"
        assert not hasattr(args, "runtime")

    def test_extract_runtime_flag_rejected(self) -> None:
        parser = build_parser()
        with pytest.raises(SystemExit):
            parser.parse_args(["extract", "v.mp4", "--runtime", "python"])

    def test_extract_custom_output(self) -> None:
        parser = build_parser()
        args = parser.parse_args(["extract", "v.mp4", "-o", "out.srt"])
        assert args.output == "out.srt"

    def test_extract_engine_mock(self) -> None:
        parser = build_parser()
        args = parser.parse_args(["extract", "v.mp4", "--engine", "mock"])
        assert args.engine == "mock"

    def test_extract_unknown_engine_rejected(self) -> None:
        parser = build_parser()
        with pytest.raises(SystemExit):
            parser.parse_args(["extract", "v.mp4", "--engine", "nonexistent"])


class TestMainEntry:
    """main 入口行为。"""

    def test_no_command_prints_help_exit_zero(self, capsys: pytest.CaptureFixture[str]) -> None:
        main([])
        captured = capsys.readouterr()
        assert "usage:" in captured.out

    def test_extract_fails_closed_to_native_cli(
        self, tmp_path: Path, capsys: pytest.CaptureFixture[str]
    ) -> None:
        video = tmp_path / "video.mp4"
        video.write_bytes(b"\x00")
        with pytest.raises(SystemExit) as exc_info:
            main(["extract", str(video), "--engine", "mock"])
        assert exc_info.value.code == 2
        captured = capsys.readouterr()
        assert "Native CLI" in captured.err
        assert "--runtime" in captured.err
        assert "build/cpp/bin/sublift" in captured.err

    def test_extract_ignores_sublift_runtime_python(
        self,
        tmp_path: Path,
        capsys: pytest.CaptureFixture[str],
        monkeypatch: pytest.MonkeyPatch,
    ) -> None:
        video = tmp_path / "video.mp4"
        video.write_bytes(b"\x00")
        monkeypatch.setenv("SUBLIFT_RUNTIME", "python")
        with pytest.raises(SystemExit) as exc_info:
            main(["extract", str(video), "--engine", "mock"])
        assert exc_info.value.code == 2
        captured = capsys.readouterr()
        assert "Native CLI" in captured.err
