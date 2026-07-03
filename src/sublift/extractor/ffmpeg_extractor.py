"""ffmpeg 帧采样实现。"""

from __future__ import annotations

import json
import subprocess
from collections.abc import Iterator
from pathlib import Path

from PIL import Image

from sublift.models import Frame


class FfmpegExtractor:
    """通过 ffmpeg 按 fps 抽帧的 Extractor 实现。"""

    def __init__(self, fps: float = 1.0) -> None:
        """初始化抽帧器。

        Args:
            fps: 采样率（每秒抽帧数），默认 1.0。
        """
        self._fps = fps

    def extract(self, video_path: Path) -> Iterator[Frame]:
        """从视频按 fps 抽帧，返回带时间戳的帧迭代器。

        Args:
            video_path: 视频文件路径。

        Yields:
            按时间顺序的 Frame，时间戳为毫秒。

        Raises:
            FileNotFoundError: 视频文件不存在。
            RuntimeError: ffmpeg 抽帧或 ffprobe 探测失败。
        """
        if not video_path.exists():
            raise FileNotFoundError(f"视频文件不存在: {video_path}")

        width, height = _probe_dimensions(video_path)
        frame_size = width * height * 3

        cmd = [
            "ffmpeg",
            "-v",
            "error",
            "-i",
            str(video_path),
            "-vf",
            f"fps={self._fps}",
            "-f",
            "image2pipe",
            "-pix_fmt",
            "rgb24",
            "-vcodec",
            "rawvideo",
            "-",
        ]

        with subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        ) as proc:
            assert proc.stdout is not None
            frame_index = 0
            while True:
                raw = proc.stdout.read(frame_size)
                if len(raw) < frame_size:
                    break
                image = Image.frombytes("RGB", (width, height), raw)
                timestamp_ms = int(frame_index / self._fps * 1000)
                yield Frame(timestamp_ms=timestamp_ms, image=image)
                frame_index += 1

            return_code = proc.wait()
            if return_code != 0:
                assert proc.stderr is not None
                stderr = proc.stderr.read().decode()
                raise RuntimeError(f"ffmpeg 抽帧失败（退出码 {return_code}）: {stderr}")


def _probe_dimensions(video_path: Path) -> tuple[int, int]:
    """用 ffprobe 获取视频分辨率。

    Args:
        video_path: 视频文件路径。

    Returns:
        (width, height) 元组。

    Raises:
        RuntimeError: ffprobe 失败或未找到视频流。
    """
    cmd = [
        "ffprobe",
        "-v",
        "error",
        "-select_streams",
        "v:0",
        "-show_entries",
        "stream=width,height",
        "-of",
        "json",
        str(video_path),
    ]
    result = subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        check=True,
    )
    data = json.loads(result.stdout)
    streams = data.get("streams", [])
    if not streams:
        raise RuntimeError(f"未找到视频流: {video_path}")
    stream = streams[0]
    return int(stream["width"]), int(stream["height"])
