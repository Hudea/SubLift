"""ffmpeg 帧采样实现。"""

from __future__ import annotations

import json
import logging
import os
import shutil
import subprocess
from collections.abc import Iterator
from pathlib import Path

from PIL import Image

from sublift.models import Frame

logger = logging.getLogger(__name__)

# GUI Process 的 PATH 可能不含 Homebrew；显式探测常见安装位置。
_FFMPEG_CANDIDATES = (
    "ffmpeg",
    "/opt/homebrew/opt/ffmpeg-full/bin/ffmpeg",
    "/opt/homebrew/bin/ffmpeg",
    "/usr/local/bin/ffmpeg",
)
_FFPROBE_CANDIDATES = (
    "ffprobe",
    "/opt/homebrew/opt/ffmpeg-full/bin/ffprobe",
    "/opt/homebrew/bin/ffprobe",
    "/usr/local/bin/ffprobe",
)


def _resolve_bin(candidates: tuple[str, ...], label: str) -> str:
    """解析可执行文件绝对路径。"""
    for name in candidates:
        if name.startswith("/"):
            if os.path.isfile(name) and os.access(name, os.X_OK):
                return name
        else:
            found = shutil.which(name)
            if found:
                return found
    raise RuntimeError(
        f"未找到 {label}，请安装 ffmpeg 并确保在 PATH 中"
        f"（已试: {', '.join(candidates)}）"
    )


def _ffmpeg_bin() -> str:
    return _resolve_bin(_FFMPEG_CANDIDATES, "ffmpeg")


def _ffprobe_bin() -> str:
    return _resolve_bin(_FFPROBE_CANDIDATES, "ffprobe")


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

        ffmpeg = _ffmpeg_bin()
        logger.info("FfmpegExtractor: ffmpeg=%s path=%s fps=%s", ffmpeg, video_path, self._fps)

        width, height = _probe_dimensions(video_path)
        frame_size = width * height * 3
        logger.info("FfmpegExtractor: probe ok %dx%d", width, height)

        cmd = [
            ffmpeg,
            # GUI/IPC 子进程无交互式 stdin；不关闭会导致 ffmpeg 阻塞读 stdin，永远无第一帧。
            "-nostdin",
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

        # stdin/stderr 必须 DEVNULL：
        # - stdin：避免 ffmpeg 等待交互输入（GUI Process 下会永久卡住）
        # - stderr：PIPE 且不读会导致缓冲区满死锁
        with subprocess.Popen(
            cmd,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
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
                raise RuntimeError(
                    f"ffmpeg 抽帧失败（退出码 {return_code}），path={video_path}"
                )


def _probe_dimensions(video_path: Path) -> tuple[int, int]:
    """用 ffprobe 获取视频分辨率。

    Args:
        video_path: 视频文件路径。

    Returns:
        (width, height) 元组。

    Raises:
        RuntimeError: ffprobe 失败或未找到视频流。
    """
    ffprobe = _ffprobe_bin()
    cmd = [
        ffprobe,
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
    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            check=True,
            timeout=60,
        )
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError(f"ffprobe 超时: {video_path}") from exc
    except FileNotFoundError as exc:
        raise RuntimeError(f"ffprobe 不可用: {ffprobe}") from exc

    data = json.loads(result.stdout)
    streams = data.get("streams", [])
    if not streams:
        raise RuntimeError(f"未找到视频流: {video_path}")
    stream = streams[0]
    return int(stream["width"]), int(stream["height"])
