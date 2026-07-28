"""CLI 入口测试。

覆盖：
- 参数解析（默认值、自定义值、engine choices）
- main 入口行为（无子命令打印 help、extract 贯通、错误处理）

mock 引擎测试不依赖真实 ffmpeg/Vision，用 monkeypatch 替换 Pipeline.run。
"""

from __future__ import annotations

from pathlib import Path

import pytest

from sublift.cli import build_parser, main
from sublift.models import SubtitleEntry


class TestParserDefaults:
    """参数解析默认值。"""

    def test_extract_defaults(self) -> None:
        """extract 默认参数：fps=5.0, engine=vision, confidence=0.5, output=output.srt。"""
        parser = build_parser()
        args = parser.parse_args(["extract", "video.mp4"])
        assert args.command == "extract"
        assert args.video == "video.mp4"
        assert args.output == "output.srt"
        assert args.fps == 5.0
        assert args.confidence == 0.5
        assert args.engine == "vision"
        assert args.script == "auto"

    def test_extract_custom_output(self) -> None:
        """-o 自定义输出路径。"""
        parser = build_parser()
        args = parser.parse_args(["extract", "v.mp4", "-o", "out.srt"])
        assert args.output == "out.srt"

    def test_extract_custom_fps(self) -> None:
        """--fps 自定义采样率。"""
        parser = build_parser()
        args = parser.parse_args(["extract", "v.mp4", "--fps", "10.0"])
        assert args.fps == 10.0

    def test_extract_custom_confidence(self) -> None:
        """--confidence 自定义阈值。"""
        parser = build_parser()
        args = parser.parse_args(["extract", "v.mp4", "--confidence", "0.8"])
        assert args.confidence == 0.8

    def test_extract_engine_vision(self) -> None:
        """--engine vision 显式指定。"""
        parser = build_parser()
        args = parser.parse_args(["extract", "v.mp4", "--engine", "vision"])
        assert args.engine == "vision"

    def test_extract_engine_mock(self) -> None:
        """--engine mock 指定。"""
        parser = build_parser()
        args = parser.parse_args(["extract", "v.mp4", "--engine", "mock"])
        assert args.engine == "mock"

    def test_extract_unknown_engine_rejected(self) -> None:
        """未知 engine 被 argparse choices 拒绝。"""
        parser = build_parser()
        with pytest.raises(SystemExit):
            parser.parse_args(["extract", "v.mp4", "--engine", "nonexistent"])

    def test_extract_script_cjk(self) -> None:
        parser = build_parser()
        args = parser.parse_args(["extract", "v.mp4", "--script", "cjk"])
        assert args.script == "cjk"


class TestMainEntry:
    """main 入口行为。"""

    def test_no_command_prints_help_exit_zero(self, capsys: pytest.CaptureFixture[str]) -> None:
        """无子命令时打印 help，正常返回（exit 0）。"""
        main([])
        captured = capsys.readouterr()
        assert "usage:" in captured.out

    def test_extract_video_not_found_exit_1(self, capsys: pytest.CaptureFixture[str]) -> None:
        """视频不存在时友好报错，exit 1。"""
        with pytest.raises(SystemExit) as exc_info:
            main(["extract", "/nonexistent/video.mp4", "--engine", "mock"])
        assert exc_info.value.code == 1
        captured = capsys.readouterr()
        assert "不存在" in captured.err

    def test_extract_vision_unavailable_exit_1(
        self,
        tmp_path: Path,
        capsys: pytest.CaptureFixture[str],
        monkeypatch: pytest.MonkeyPatch,
    ) -> None:
        """Vision 不可用时友好报错，exit 1。"""
        video = tmp_path / "video.mp4"
        video.write_bytes(b"\x00")
        monkeypatch.setattr("sublift.cli.is_vision_available", lambda: False)
        with pytest.raises(SystemExit) as exc_info:
            main(["extract", str(video), "--engine", "vision"])
        assert exc_info.value.code == 1
        captured = capsys.readouterr()
        assert "uv sync --extra vision" in captured.err

    def test_extract_paddle_initialization_failure_is_actionable(
        self,
        tmp_path: Path,
        capsys: pytest.CaptureFixture[str],
        monkeypatch: pytest.MonkeyPatch,
    ) -> None:
        """Paddle 已安装但模型初始化失败时，不暴露 traceback。"""
        video = tmp_path / "video.mp4"
        video.write_bytes(b"\x00")

        from sublift.extractor.ffmpeg_extractor import VideoInfo

        monkeypatch.setattr(
            "sublift.extractor.ffmpeg_extractor.probe_video",
            lambda v: VideoInfo(320, 240, 5000),
        )
        monkeypatch.setattr("sublift.cli.is_paddle_available", lambda: True)

        def raise_initialization_error() -> None:
            raise RuntimeError("模型下载失败：网络不可达")

        monkeypatch.setattr("sublift.cli.PaddleOcrEngine", raise_initialization_error)

        with pytest.raises(SystemExit) as exc_info:
            main(["extract", str(video), "--engine", "paddle"])

        assert exc_info.value.code == 1
        captured = capsys.readouterr()
        assert "PaddleOCR 初始化失败" in captured.err
        assert "模型下载失败：网络不可达" in captured.err
        assert "检查网络后重试" in captured.err
        assert "uv run --extra paddle python -c" in captured.err
        assert "Traceback" not in captured.out + captured.err

    def test_extract_mock_engine_writes_srt(
        self,
        tmp_path: Path,
        monkeypatch: pytest.MonkeyPatch,
        capsys: pytest.CaptureFixture[str],
    ) -> None:
        """--engine mock 贯通：伪造 Pipeline 输出，验证 SRT 文件写入。"""
        video = tmp_path / "video.mp4"
        video.write_bytes(b"\x00")
        output = tmp_path / "out.srt"

        fake_entries = [
            SubtitleEntry(start_ms=0, end_ms=1000, text="[mock subtitle]"),
            SubtitleEntry(start_ms=2000, end_ms=3000, text="[mock subtitle]"),
        ]

        from sublift.extractor.ffmpeg_extractor import VideoInfo

        monkeypatch.setattr(
            "sublift.extractor.ffmpeg_extractor.probe_video", lambda v: VideoInfo(320, 240, 5000)
        )
        monkeypatch.setattr(
            "sublift.extractor.ffmpeg_extractor.FfmpegExtractor.extract", lambda self, v: iter([])
        )
        monkeypatch.setattr("sublift.cli.Pipeline.feed", lambda self, f: None)
        monkeypatch.setattr("sublift.cli.Pipeline.finalize", lambda self: fake_entries)
        monkeypatch.setattr("sublift.cli.is_vision_available", lambda: False)

        main(["extract", str(video), "-o", str(output), "--engine", "mock"])

        content = output.read_text(encoding="utf-8")
        assert "1\n" in content
        assert "00:00:00,000 --> 00:00:01,000" in content
        assert "[mock subtitle]" in content
        assert "2\n" in content
        assert "00:00:02,000 --> 00:00:03,000" in content

        captured = capsys.readouterr()
        assert "完成" in captured.out
        assert "2 条字幕" in captured.out

    def test_extract_cli_progress_output(
        self,
        tmp_path: Path,
        monkeypatch: pytest.MonkeyPatch,
        capsys: pytest.CaptureFixture[str],
    ) -> None:
        """验证 TTY 和非 TTY 重定向时的进度输出。"""
        video = tmp_path / "video.mp4"
        video.write_bytes(b"\x00")
        output = tmp_path / "out.srt"

        from PIL import Image

        from sublift.extractor.ffmpeg_extractor import VideoInfo
        from sublift.models import Frame

        # 1. 模拟 TTY
        monkeypatch.setattr("sys.stdout.isatty", lambda: True)
        monkeypatch.setattr(
            "sublift.extractor.ffmpeg_extractor.probe_video", lambda v: VideoInfo(320, 240, 400)
        )
        mock_frames = [
            Frame(0, Image.new("RGB", (10, 10))),
            Frame(1000, Image.new("RGB", (10, 10))),
        ]

        class MockPipeline:
            def __init__(self, *args: object, **kwargs: object) -> None:
                self.processed_count = 0

            def _reset_streaming_state(self) -> None:
                self.processed_count = 0

            def feed(self, frame: object) -> None:
                self.processed_count += 1
                return None

            def ocr_segment(self, event: object) -> None:
                return None

            def finalize(self) -> list[SubtitleEntry]:
                return []

        monkeypatch.setattr("sublift.cli.Pipeline", MockPipeline)
        monkeypatch.setattr(
            "sublift.extractor.ffmpeg_extractor.FfmpegExtractor.extract",
            lambda self, v: iter(mock_frames),
        )

        main(["extract", str(video), "-o", str(output), "--engine", "mock"])
        captured = capsys.readouterr()
        assert "\r[2/3] 提取与识别:" in captured.out

        # 2. 模拟非 TTY (重定向)
        monkeypatch.setattr("sys.stdout.isatty", lambda: False)
        main(["extract", str(video), "-o", str(output), "--engine", "mock"])
        captured2 = capsys.readouterr()
        assert "\r[2/3] 提取与识别:" not in captured2.out
        assert "[2/3] 提取与识别: 100%" in captured2.out
