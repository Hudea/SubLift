"""CLI 入口测试。

覆盖：
- 参数解析（默认值、自定义值、engine choices, runtime choices）
- main 入口行为（无子命令打印 help、extract 贯通、错误处理）
- C++ / Python 双轨 Cutover 路由与环境覆盖

mock 引擎测试不依赖真实 ffmpeg/Vision，用 monkeypatch 替换 Pipeline.run。
"""

from __future__ import annotations

from pathlib import Path

import pytest

from sublift.cli import _find_sublift_worker_bin, build_parser, main
from sublift.models import SubtitleEntry


class TestParserDefaults:
    """参数解析默认值。"""

    def test_extract_defaults(self) -> None:
        """extract 默认参数。"""
        parser = build_parser()
        args = parser.parse_args(["extract", "video.mp4"])
        assert args.command == "extract"
        assert args.video == "video.mp4"
        assert args.output == "output.srt"
        assert args.fps == 5.0
        assert args.confidence == 0.5
        assert args.engine == "vision"
        assert args.runtime is None
        assert args.script == "auto"

    def test_extract_custom_runtime_python(self) -> None:
        """--runtime python 参数解析。"""
        parser = build_parser()
        args = parser.parse_args(["extract", "v.mp4", "--runtime", "python"])
        assert args.runtime == "python"

    def test_extract_custom_runtime_cpp(self) -> None:
        """--runtime cpp 参数解析。"""
        parser = build_parser()
        args = parser.parse_args(["extract", "v.mp4", "--runtime", "cpp"])
        assert args.runtime == "cpp"

    def test_extract_unknown_runtime_rejected(self) -> None:
        """未知 runtime 被 argparse choices 拒绝。"""
        parser = build_parser()
        with pytest.raises(SystemExit):
            parser.parse_args(["extract", "v.mp4", "--runtime", "invalid"])

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
            main(["extract", str(video), "--engine", "vision", "--runtime", "python"])
        assert exc_info.value.code == 1

        captured = capsys.readouterr()
        assert "Apple Vision 不可用" in captured.err

    def test_extract_mock_engine_success(
        self,
        tmp_path: Path,
        capsys: pytest.CaptureFixture[str],
        monkeypatch: pytest.MonkeyPatch,
    ) -> None:
        """mock 引擎在 Python runtime 端到端成功运行并输出 srt。"""
        video = tmp_path / "video.mp4"
        video.write_bytes(b"\x00")
        output = tmp_path / "out.srt"

        from PIL import Image

        from sublift.extractor.ffmpeg_extractor import VideoInfo
        from sublift.models import Frame

        monkeypatch.setattr(
            "sublift.extractor.ffmpeg_extractor.probe_video", lambda v: VideoInfo(320, 240, 4000)
        )
        mock_frames = [
            Frame(0, Image.new("RGB", (10, 10))),
            Frame(1000, Image.new("RGB", (10, 10))),
        ]
        monkeypatch.setattr(
            "sublift.extractor.ffmpeg_extractor.FfmpegExtractor.extract",
            lambda self, v: iter(mock_frames),
        )

        entries = [
            SubtitleEntry(start_ms=0, end_ms=1000, text="[mock subtitle]"),
            SubtitleEntry(start_ms=2000, end_ms=3000, text="[mock subtitle]"),
        ]

        class MockPipeline:
            def __init__(self, *args: object, **kwargs: object) -> None:
                self.processed_count = 0

            def feed(self, frame: object) -> None:
                self.processed_count += 1
                return None

            def ocr_segment(self, event: object) -> None:
                return None

            def finalize(self) -> list[SubtitleEntry]:
                return entries

        monkeypatch.setattr("sublift.cli.Pipeline", MockPipeline)
        monkeypatch.setattr("sublift.cli.is_vision_available", lambda: False)

        main(["extract", str(video), "-o", str(output), "--engine", "mock", "--runtime", "python"])

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

        main(["extract", str(video), "-o", str(output), "--engine", "mock", "--runtime", "python"])
        captured = capsys.readouterr()
        assert "\r[2/3] 提取与识别:" in captured.out

        monkeypatch.setattr("sys.stdout.isatty", lambda: False)
        main(["extract", str(video), "-o", str(output), "--engine", "mock", "--runtime", "python"])
        captured2 = capsys.readouterr()
        assert "\r[2/3] 提取与识别:" not in captured2.out
        assert "[2/3] 提取与识别: 100%" in captured2.out


class TestCliCutoverRouting:
    """CLI Phase 6.6 Cutover 路由与环境覆盖测试。"""

    def test_find_sublift_worker_bin_from_env(
        self, monkeypatch: pytest.MonkeyPatch, tmp_path: Path
    ) -> None:
        fake_bin = tmp_path / "sublift_worker"
        fake_bin.write_bytes(b"#!/bin/sh\nexit 0\n")
        fake_bin.chmod(0o755)

        monkeypatch.setenv("SUBLIFT_WORKER_PATH", str(fake_bin))
        found = _find_sublift_worker_bin()
        assert found == fake_bin

    def test_cli_defaults_to_cpp_runtime(
        self,
        tmp_path: Path,
        monkeypatch: pytest.MonkeyPatch,
    ) -> None:
        video = tmp_path / "video.mp4"
        video.write_bytes(b"\x00")
        output = tmp_path / "out.srt"

        called = {"cpp": False}

        def mock_cpp(*args: object, **kwargs: object) -> None:
            called["cpp"] = True

        monkeypatch.setattr("sublift.cli._run_extract_cpp", mock_cpp)

        main(["extract", str(video), "-o", str(output), "--engine", "mock"])

        assert called["cpp"] is True

    def test_cli_env_var_sublift_runtime_routes_to_python_rollback(
        self,
        tmp_path: Path,
        monkeypatch: pytest.MonkeyPatch,
    ) -> None:
        video = tmp_path / "video.mp4"
        video.write_bytes(b"\x00")
        output = tmp_path / "out.srt"

        monkeypatch.setenv("SUBLIFT_RUNTIME", "python")

        called = {"python": False}

        def mock_python(*args: object, **kwargs: object) -> None:
            called["python"] = True

        monkeypatch.setattr("sublift.cli._run_extract_python", mock_python)

        main(["extract", str(video), "-o", str(output), "--engine", "mock"])

        assert called["python"] is True

    def test_cli_paddle_cpp_unavailable_throws(
        self,
        tmp_path: Path,
        capsys: pytest.CaptureFixture[str],
        monkeypatch: pytest.MonkeyPatch,
    ) -> None:
        video = tmp_path / "video.mp4"
        video.write_bytes(b"\x00")
        output = tmp_path / "out.srt"

        monkeypatch.setattr("sublift.cli.probe_cpp_paddle_available", lambda: False)

        from sublift.runtime import RuntimePolicyError
        with pytest.raises(RuntimePolicyError, match="Paddle C\\+\\+ engine is unavailable"):
            main([
                "extract", str(video), "-o", str(output),
                "--engine", "paddle", "--runtime", "cpp"
            ])

    def test_cli_cpp_missing_binary_exits_1(
        self,
        tmp_path: Path,
        capsys: pytest.CaptureFixture[str],
        monkeypatch: pytest.MonkeyPatch,
    ) -> None:
        video = tmp_path / "video.mp4"
        video.write_bytes(b"\x00")

        monkeypatch.setattr("sublift.cli._find_sublift_worker_bin", lambda: None)

        with pytest.raises(SystemExit) as exc_info:
            main(["extract", str(video), "--engine", "mock", "--runtime", "cpp"])
        assert exc_info.value.code == 1

        captured = capsys.readouterr()
        assert "未找到 C++ worker 可执行文件" in captured.err

    def test_cli_cpp_worker_dispatch_mock(
        self,
        tmp_path: Path,
        capsys: pytest.CaptureFixture[str],
        monkeypatch: pytest.MonkeyPatch,
    ) -> None:
        video = tmp_path / "video.mp4"
        video.write_bytes(b"\x00")
        output = tmp_path / "out.srt"

        called_cpp = {"run": False}

        def mock_run_cpp(*args: object, **kwargs: object) -> None:
            called_cpp["run"] = True
            output.write_text("1\n00:00:00,000 --> 00:00:01,000\n[cpp mock]\n\n", encoding="utf-8")

        monkeypatch.setattr("sublift.cli._run_extract_cpp", mock_run_cpp)

        main(["extract", str(video), "-o", str(output), "--engine", "mock", "--runtime", "cpp"])

        assert called_cpp["run"] is True
        assert output.exists()
        assert "[cpp mock]" in output.read_text(encoding="utf-8")

    def test_cli_cpp_start_job_includes_subtitle_profile(
        self,
        tmp_path: Path,
        monkeypatch: pytest.MonkeyPatch,
    ) -> None:
        """GATE-07: C++ path must send subtitle_profile (script) on start_job."""
        from typing import Any

        from sublift.cli import _run_extract_cpp
        from sublift.runtime import ResolutionSource, WorkerChoice

        video = tmp_path / "video.mp4"
        video.write_bytes(b"\x00")
        output = tmp_path / "out.srt"
        worker = tmp_path / "sublift_worker"
        worker.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        worker.chmod(0o755)

        captured: list[dict[str, Any]] = []

        class FakeProc:
            def poll(self) -> int | None:
                return None

            def terminate(self) -> None:
                return None

            def wait(self, timeout: float | None = None) -> int:
                return 0

            def kill(self) -> None:
                return None

        class FakeSocket:
            def connect(self, path: str) -> None:
                return None

            def sendall(self, data: bytes) -> None:
                return None

            def recv(self, n: int) -> bytes:
                return b""

            def close(self) -> None:
                return None

        def fake_write(_sock: object, payload: dict[str, Any]) -> None:
            captured.append(payload)

        responses: list[dict[str, Any]] = [
            {"type": "bye", "protocol_version": 1, "runtime": "cpp", "engines": ["mock"]},
            {
                "type": "entries",
                "video_id": "cli_job",
                "entries": [
                    {
                        "start_ms": 0,
                        "end_ms": 1000,
                        "text": "hi",
                        "confidence": 1.0,
                    }
                ],
            },
            {"type": "done", "video_id": "cli_job", "ok": True},
        ]
        response_iter = iter(responses)

        def fake_read(_sock: object) -> dict[str, Any]:
            return next(response_iter)

        real_exists = Path.exists

        def fake_exists(self: Path) -> bool:
            # Pretend UDS path appears immediately after worker spawn
            if str(self).endswith(".sock"):
                return True
            return real_exists(self)

        monkeypatch.setattr("sublift.cli._find_sublift_worker_bin", lambda: worker)
        monkeypatch.setattr("sublift.cli.subprocess.Popen", lambda *a, **k: FakeProc())
        monkeypatch.setattr("sublift.cli.socket.socket", lambda *a, **k: FakeSocket())
        monkeypatch.setattr("sublift.cli._write_framed_json", fake_write)
        monkeypatch.setattr("sublift.cli._read_framed_json", fake_read)
        monkeypatch.setattr(Path, "exists", fake_exists)

        _run_extract_cpp(
            video=video,
            output=output,
            fps=5.0,
            confidence=0.5,
            script="cjk",
            choice=WorkerChoice(
                runtime="cpp", engine="mock", resolved_via=ResolutionSource.PRODUCT_DEFAULT
            ),
        )

        start_jobs = [m for m in captured if m.get("type") == "start_job"]
        assert len(start_jobs) == 1
        profile = start_jobs[0].get("subtitle_profile")
        assert isinstance(profile, dict)
        assert profile.get("script") == "cjk"
