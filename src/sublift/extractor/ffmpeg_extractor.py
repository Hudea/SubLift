"""ffmpeg 帧采样实现。"""

from __future__ import annotations

import json
import logging
import os
import shutil
import subprocess
import tempfile
import threading
from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path

from PIL import Image

from sublift.models import Frame

# 失败时附带的 stderr 尾部最大字符数
_STDERR_TAIL_CHARS = 2000

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


@dataclass(frozen=True)
class VideoInfo:
    """视频探测元数据信息。"""

    width: int
    height: int
    duration_ms: int


def probe_video(video_path: Path) -> VideoInfo:
    """探测视频的尺寸和时长。"""
    width, height = _probe_dimensions(video_path)
    duration_ms = probe_duration_ms(video_path)
    return VideoInfo(width=width, height=height, duration_ms=duration_ms)


class FfmpegExtractor:
    """通过 ffmpeg 按 fps 抽帧的 Extractor 实现。"""

    def __init__(self, fps: float = 1.0) -> None:
        """初始化抽帧器。

        Args:
            fps: 采样率（每秒抽帧数），默认 1.0。
        """
        self._fps = fps
        self._proc: subprocess.Popen[bytes] | None = None
        self._cancelled = threading.Event()

    def cancel(self) -> None:
        """强制终止当前的 ffmpeg 抽帧子进程。"""
        self._cancelled.set()
        if self._proc is not None:
            try:
                self._proc.terminate()
                self._proc.kill()
            except Exception:
                pass

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
        if self._cancelled.is_set():
            return

        if not video_path.exists():
            raise FileNotFoundError(f"视频文件不存在: {video_path}")

        ffmpeg = _ffmpeg_bin()
        logger.info("FfmpegExtractor: ffmpeg=%s path=%s fps=%s", ffmpeg, video_path, self._fps)

        if self._cancelled.is_set():
            return

        width, height = _probe_dimensions(video_path)
        frame_size = width * height * 3
        logger.info("FfmpegExtractor: probe ok %dx%d", width, height)

        if self._cancelled.is_set():
            return

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

        # stdin=DEVNULL：避免 ffmpeg 等待交互输入（GUI Process 下会永久卡住）
        # stderr 写入临时文件（非 PIPE）：避免缓冲区满死锁，失败时仍保留诊断尾部
        stderr_path: Path | None = None
        try:
            with tempfile.NamedTemporaryFile(
                prefix="sublift-ffmpeg-stderr-",
                suffix=".log",
                delete=False,
            ) as err_file:
                stderr_path = Path(err_file.name)

            if self._cancelled.is_set():
                return

            with (
                stderr_path.open("wb") as stderr_fh,
                subprocess.Popen(
                    cmd,
                    stdin=subprocess.DEVNULL,
                    stdout=subprocess.PIPE,
                    stderr=stderr_fh,
                ) as proc,
            ):
                self._proc = proc
                if self._cancelled.is_set():
                    try:
                        proc.terminate()
                        proc.kill()
                    except Exception:
                        pass
                    return

                try:
                    assert proc.stdout is not None
                    frame_index = 0
                    while True:
                        if self._cancelled.is_set():
                            break
                        raw = proc.stdout.read(frame_size)
                        if len(raw) < frame_size:
                            break
                        image = Image.frombytes("RGB", (width, height), raw)
                        timestamp_ms = int(frame_index / self._fps * 1000)
                        yield Frame(timestamp_ms=timestamp_ms, image=image)
                        frame_index += 1
                finally:
                    self._proc = None

                return_code = proc.wait()
                if not self._cancelled.is_set() and return_code != 0:
                    tail = _read_stderr_tail(stderr_path)
                    detail = f"：{tail}" if tail else ""
                    raise RuntimeError(
                        f"ffmpeg 抽帧失败（退出码 {return_code}），"
                        f"path={video_path}{detail}"
                    )
        finally:
            if stderr_path is not None:
                try:
                    stderr_path.unlink(missing_ok=True)
                except OSError:
                    logger.debug("cleanup ffmpeg stderr log failed: %s", stderr_path)


def _read_stderr_tail(path: Path, max_chars: int = _STDERR_TAIL_CHARS) -> str:
    """读取 stderr 日志尾部并压成单行摘要。"""
    try:
        text = path.read_text(encoding="utf-8", errors="replace").strip()
    except OSError:
        return ""
    if not text:
        return ""
    if len(text) > max_chars:
        text = text[-max_chars:]
    return " ".join(text.split())


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
            check=False,
            timeout=60,
        )
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError(f"ffprobe 超时: {video_path}") from exc
    except FileNotFoundError as exc:
        raise RuntimeError(f"ffprobe 不可用: {ffprobe}") from exc

    if result.returncode != 0:
        tail = " ".join((result.stderr or "").split())
        if len(tail) > _STDERR_TAIL_CHARS:
            tail = tail[-_STDERR_TAIL_CHARS:]
        detail = f"：{tail}" if tail else ""
        raise RuntimeError(
            f"ffprobe 失败（退出码 {result.returncode}），path={video_path}{detail}"
        )

    data = json.loads(result.stdout)
    streams = data.get("streams", [])
    if not streams:
        raise RuntimeError(f"未找到视频流: {video_path}")
    stream = streams[0]
    return int(stream["width"]), int(stream["height"])


def probe_duration_ms(video_path: Path) -> int:
    """用 ffprobe 获取视频时长（毫秒）。

    Args:
        video_path: 视频文件路径。

    Returns:
        时长（毫秒），无法获取时返回 0。
    """
    if not video_path.exists():
        return 0
    ffprobe = _ffprobe_bin()
    cmd = [
        ffprobe,
        "-v",
        "error",
        "-show_entries",
        "format=duration",
        "-of",
        "default=noprint_wrappers=1:nokey=1",
        str(video_path),
    ]
    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            check=False,
            timeout=10,
        )
        if result.returncode == 0 and result.stdout.strip():
            val = float(result.stdout.strip())
            return int(val * 1000)
    except Exception:
        logger.exception("ffprobe probe_duration_ms 失败")
    return 0
