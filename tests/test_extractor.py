"""帧采样模块（extractor）测试。

Protocol 单测默认运行；ffmpeg 抽帧测试标 integration，需系统 ffmpeg。
"""

from __future__ import annotations

import subprocess
from pathlib import Path

import pytest

from sublift.extractor.base import Extractor
from sublift.extractor.ffmpeg_extractor import FfmpegExtractor


class TestExtractorProtocol:
    """Extractor Protocol 单测，不依赖外部资源。"""

    def test_ffmpeg_extractor_is_extractor(self) -> None:
        """FfmpegExtractor 满足 Extractor Protocol。"""
        extractor = FfmpegExtractor(fps=1.0)
        assert isinstance(extractor, Extractor)

    def test_ffmpeg_extractor_default_fps(self) -> None:
        extractor = FfmpegExtractor()
        assert extractor._fps == 1.0

    def test_ffmpeg_extractor_custom_fps(self) -> None:
        extractor = FfmpegExtractor(fps=2.0)
        assert extractor._fps == 2.0

    def test_extract_nonexistent_file_raises(self) -> None:
        extractor = FfmpegExtractor()
        with pytest.raises(FileNotFoundError):
            next(extractor.extract(Path("/nonexistent/video.mp4")))

    def test_extract_invalid_file_includes_ffmpeg_stderr(
        self, tmp_path: Path
    ) -> None:
        """损坏/非视频输入失败时应附带 stderr 诊断，而非仅有退出码。"""
        bad = tmp_path / "not_a_video.mp4"
        bad.write_bytes(b"this is not a media file")
        extractor = FfmpegExtractor(fps=1.0)
        with pytest.raises(RuntimeError) as ei:
            list(extractor.extract(bad))
        msg = str(ei.value)
        assert "退出码" in msg
        # 尾部应含 ffprobe/ffmpeg 真实错误（不仅是数字退出码）
        assert len(msg) > len("ffprobe 失败（退出码 1），path=")
        assert any(
            token in msg.lower()
            for token in ("invalid", "error", "moov", "failed", "not", "解析", "格式")
        )


def _generate_test_video(path: Path, duration: float = 3.0, fps: float = 2.0) -> None:
    """用 ffmpeg lavfi 生成测试视频。

    Args:
        path: 输出视频路径。
        duration: 视频时长（秒）。
        fps: 视频帧率。
    """
    width, height = 320, 240
    cmd = [
        "ffmpeg",
        "-v",
        "error",
        "-f",
        "lavfi",
        "-i",
        f"testsrc=duration={duration}:size={width}x{height}:rate={fps}",
        "-c:v",
        "libx264",
        "-pix_fmt",
        "yuv420p",
        str(path),
        "-y",
    ]
    subprocess.run(cmd, capture_output=True, check=True)


@pytest.mark.integration
class TestFfmpegExtractorIntegration:
    """ffmpeg 抽帧集成测试，需系统 ffmpeg。"""

    def test_extract_default_fps(self, tmp_path: Path) -> None:
        """3 秒视频以 1 fps 抽帧应得到 3 帧。"""
        video = tmp_path / "test.mp4"
        _generate_test_video(video, duration=3.0, fps=2.0)

        extractor = FfmpegExtractor(fps=1.0)
        frames = list(extractor.extract(video))

        assert len(frames) == 3
        assert [f.timestamp_ms for f in frames] == [0, 1000, 2000]

    def test_extract_frame_dimensions(self, tmp_path: Path) -> None:
        """抽帧尺寸应与视频分辨率一致。"""
        video = tmp_path / "test.mp4"
        _generate_test_video(video, duration=2.0, fps=2.0)

        extractor = FfmpegExtractor(fps=1.0)
        frames = list(extractor.extract(video))

        for frame in frames:
            assert frame.image.size == (320, 240)
            assert frame.image.mode == "RGB"

    def test_extract_higher_fps(self, tmp_path: Path) -> None:
        """3 秒视频以 2 fps 抽帧应得到 6 帧。"""
        video = tmp_path / "test.mp4"
        _generate_test_video(video, duration=3.0, fps=2.0)

        extractor = FfmpegExtractor(fps=2.0)
        frames = list(extractor.extract(video))

        assert len(frames) == 6
        assert frames[0].timestamp_ms == 0
        assert frames[1].timestamp_ms == 500
        assert frames[-1].timestamp_ms == 2500

    def test_probe_duration_ms(self, tmp_path: Path) -> None:
        """验证时长探测。"""
        video = tmp_path / "test.mp4"
        _generate_test_video(video, duration=3.0, fps=2.0)
        from sublift.extractor.ffmpeg_extractor import probe_duration_ms
        assert probe_duration_ms(video) == 3000

    def test_ffmpeg_extractor_cancel_midway(self, tmp_path: Path) -> None:
        """中途取消测试。"""
        video = tmp_path / "test.mp4"
        _generate_test_video(video, duration=5.0, fps=2.0)
        extractor = FfmpegExtractor(fps=1.0)

        frames = []
        for frame in extractor.extract(video):
            frames.append(frame)
            if len(frames) == 2:
                extractor.cancel()
        assert len(frames) == 2

    def test_ffmpeg_extractor_cancel_before_extract(self, tmp_path: Path) -> None:
        """在开始抽取前取消测试。"""
        video = tmp_path / "test.mp4"
        _generate_test_video(video, duration=3.0, fps=2.0)
        extractor = FfmpegExtractor(fps=1.0)
        extractor.cancel()

        frames = list(extractor.extract(video))
        assert len(frames) == 0
