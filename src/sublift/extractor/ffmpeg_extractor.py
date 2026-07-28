"""ffmpeg 帧采样实现。

支持可选 source-frame ``output_crop``（feat-038）：在 fps 采样后 exact crop，
仅将 ROI raw RGB 写入 stdout。这是 crop-before-Python，不是 codec 级 ROI decode。
坐标空间见 docs/design/roi-data-path.md。
"""

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
from typing import TYPE_CHECKING, Any

from PIL import Image

from sublift.models import BoundingBox, Frame

if TYPE_CHECKING:
    from sublift.diagnostics.performance import PerformanceRecorder

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
        f"未找到 {label}，请安装 ffmpeg 并确保在 PATH 中（已试: {', '.join(candidates)}）"
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


@dataclass(frozen=True)
class SourceFrameInfo:
    """source-frame 探测结果（含显示变换是否可信任）。

    ``display_transform_ok`` 为 False 时，自动 ROI 必须回退全帧；
    本结构不实现旋转坐标重映射。
    """

    width: int
    height: int
    display_transform_ok: bool
    transform_note: str | None = None


def validate_output_crop(
    crop: BoundingBox,
    source_width: int,
    source_height: int,
) -> None:
    """严格校验 source-frame output_crop；非法立即失败，禁止 clamp。

    Args:
        crop: source-frame 坐标下的裁剪区。
        source_width: 源视频宽度。
        source_height: 源视频高度。

    Raises:
        ValueError: 坐标/尺寸非法或越界；消息含 source 与 requested。
    """
    x, y, w, h = crop.x, crop.y, crop.width, crop.height
    requested = f"[{x}, {y}, {w}, {h}]"
    source = f"{source_width}x{source_height}"
    if x < 0 or y < 0:
        raise ValueError(f"output_crop 的 x/y 不能为负：source={source} requested={requested}")
    if w <= 0 or h <= 0:
        raise ValueError(
            f"output_crop 的 width/height 必须为正：source={source} requested={requested}"
        )
    if x + w > source_width or y + h > source_height:
        raise ValueError(f"output_crop 越界：source={source} requested={requested}")


def probe_video(video_path: Path) -> VideoInfo:
    """探测视频的尺寸和时长。"""
    info = probe_source_frame(video_path)
    duration_ms = probe_duration_ms(video_path)
    return VideoInfo(width=info.width, height=info.height, duration_ms=duration_ms)


def probe_source_frame(video_path: Path) -> SourceFrameInfo:
    """探测 source-frame 尺寸与显示变换是否恒等/已验证。"""
    return _probe_source_frame(video_path)


class FfmpegExtractor:
    """通过 ffmpeg 按 fps 抽帧的 Extractor 实现。

    可选 ``output_crop``（source-frame 坐标）在 RGB 输出前 exact crop，
    使 Frame.image 处于 frame-local 坐标。
    """

    def __init__(
        self,
        fps: float = 1.0,
        *,
        output_crop: BoundingBox | None = None,
        source_info: SourceFrameInfo | None = None,
        performance_recorder: PerformanceRecorder | None = None,
    ) -> None:
        """初始化抽帧器。

        Args:
            fps: 采样率（每秒抽帧数），默认 1.0。
            output_crop: 可选 source-frame 裁剪区。None 输出全帧；
                非 None 时在 spawn 前严格校验，失败立即报错，不静默全帧。
            source_info: 可选预探测结果（来自 plan_frame_io），避免二次 ffprobe。
            performance_recorder: 可选性能记录器（feat-037/038）。
                记录 probe、spawn→首帧、extract_wait（decode+filter+RGB+pipe）
                与 raw_output_bytes；不把 extract_wait 标为纯 codec decode。
        """
        self._fps = fps
        self._output_crop = output_crop
        self._source_info = source_info
        self._proc: subprocess.Popen[bytes] | None = None
        self._cancelled = threading.Event()
        self._perf = performance_recorder

    @property
    def output_crop(self) -> BoundingBox | None:
        """source-frame crop；None 表示全帧输出。"""
        return self._output_crop

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
            按时间顺序的 Frame，时间戳为毫秒。ROI 路径下 image 为 frame-local。

        Raises:
            FileNotFoundError: 视频文件不存在。
            ValueError: output_crop 几何非法。
            RuntimeError: ffmpeg 抽帧或 ffprobe 探测失败。
        """
        # 延迟导入避免与 frame_io 循环依赖（frame_io 引用 validate/probe）
        from sublift.extractor.frame_io import build_output_vf

        if self._cancelled.is_set():
            return

        if not video_path.exists():
            raise FileNotFoundError(f"视频文件不存在: {video_path}")

        ffmpeg = _ffmpeg_bin()
        crop = self._output_crop
        logger.info(
            "FfmpegExtractor: ffmpeg=%s path=%s fps=%s output_crop=%s",
            ffmpeg,
            video_path,
            self._fps,
            None if crop is None else [crop.x, crop.y, crop.width, crop.height],
        )

        if self._cancelled.is_set():
            return

        perf = self._perf
        if self._source_info is not None:
            source_w = self._source_info.width
            source_h = self._source_info.height
        elif perf is not None:
            with perf.span("probe"):
                source_w, source_h = _probe_dimensions(video_path)
        else:
            source_w, source_h = _probe_dimensions(video_path)

        if crop is not None:
            validate_output_crop(crop, source_w, source_h)
            out_w, out_h = crop.width, crop.height
            output_mode = "roi_rgb"
        else:
            out_w, out_h = source_w, source_h
            output_mode = "full_rgb"
        vf = build_output_vf(self._fps, crop)

        frame_size = out_w * out_h * 3
        logger.info(
            "FfmpegExtractor: probe ok source=%dx%d output=%dx%d mode=%s",
            source_w,
            source_h,
            out_w,
            out_h,
            output_mode,
        )

        if perf is not None:
            perf.set_workload(
                {
                    "source_width": source_w,
                    "source_height": source_h,
                    "output_width": out_w,
                    "output_height": out_h,
                    "output_mode": output_mode,
                    "source_region_box": (
                        None if crop is None else [crop.x, crop.y, crop.width, crop.height]
                    ),
                    "raw_bytes_per_frame": frame_size,
                }
            )

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
            vf,
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

            if perf is not None:
                perf.mark_spawn()

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
                    if perf is not None:
                        perf.note_incomplete("cancelled")
                    return

                try:
                    assert proc.stdout is not None
                    frame_index = 0
                    while True:
                        if self._cancelled.is_set():
                            if perf is not None:
                                perf.note_incomplete("cancelled")
                            break
                        if perf is not None:
                            t0 = perf.now_ns()
                            raw = proc.stdout.read(frame_size)
                            wait_ns = perf.now_ns() - t0
                            # extract_wait = decode + filter + RGB + pipe output（非纯解码）
                            perf.add_stage_ns("extract_wait", wait_ns)
                        else:
                            raw = proc.stdout.read(frame_size)
                        if len(raw) < frame_size:
                            break
                        if perf is not None:
                            perf.incr("raw_output_bytes", len(raw))
                            if frame_index == 0:
                                perf.mark_first_frame()
                            perf.incr("frame_count")
                            with perf.span("frame_materialize"):
                                image = Image.frombytes("RGB", (out_w, out_h), raw)
                        else:
                            image = Image.frombytes("RGB", (out_w, out_h), raw)
                        timestamp_ms = int(frame_index / self._fps * 1000)
                        yield Frame(timestamp_ms=timestamp_ms, image=image)
                        frame_index += 1
                finally:
                    self._proc = None

                return_code = proc.wait()
                if not self._cancelled.is_set() and return_code != 0:
                    if perf is not None:
                        perf.note_incomplete(f"ffmpeg_exit_{return_code}")
                    tail = _read_stderr_tail(stderr_path)
                    detail = f"：{tail}" if tail else ""
                    raise RuntimeError(
                        f"ffmpeg 抽帧失败（退出码 {return_code}），path={video_path}{detail}"
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
    """用 ffprobe 获取视频分辨率。"""
    info = _probe_source_frame(video_path)
    return info.width, info.height


def _probe_source_frame(video_path: Path) -> SourceFrameInfo:
    """用 ffprobe 获取 source-frame 尺寸与显示变换状态。

    保守策略：无法确认恒等旋转/显示矩阵时 ``display_transform_ok=False``，
    调用方应禁用自动 ROI。
    """
    ffprobe = _ffprobe_bin()
    cmd = [
        ffprobe,
        "-v",
        "error",
        "-select_streams",
        "v:0",
        "-show_entries",
        "stream=width,height,tags,side_data_list",
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
        raise RuntimeError(f"ffprobe 失败（退出码 {result.returncode}），path={video_path}{detail}")

    try:
        data = json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"ffprobe JSON 解析失败: {video_path}") from exc

    streams = data.get("streams", [])
    if not streams:
        raise RuntimeError(f"未找到视频流: {video_path}")
    stream = streams[0]
    try:
        width = int(stream["width"])
        height = int(stream["height"])
    except (KeyError, TypeError, ValueError) as exc:
        raise RuntimeError(f"ffprobe 缺少 width/height: {video_path}") from exc

    ok, note = _assess_display_transform(stream)
    return SourceFrameInfo(
        width=width,
        height=height,
        display_transform_ok=ok,
        transform_note=note,
    )


def _assess_display_transform(stream: dict[str, Any]) -> tuple[bool, str | None]:
    """判断显示/旋转变换是否恒等且可信任。

    Returns:
        (ok, note)：ok=True 仅当明确无非恒等旋转且无 Display Matrix 非恒等迹象。
    """
    tags = stream.get("tags")
    if isinstance(tags, dict):
        rotate = tags.get("rotate")
        if rotate is not None and str(rotate).strip() not in ("", "0"):
            return False, f"stream.tags.rotate={rotate!r}"

    side_data = stream.get("side_data_list")
    if side_data is None:
        return True, None
    if not isinstance(side_data, list):
        return False, "stream.side_data_list 不可解析"

    for item in side_data:
        if not isinstance(item, dict):
            return False, "stream.side_data_list 项不可解析"
        side_type = str(item.get("side_data_type", "")).lower()
        if "display matrix" not in side_type and "displaymatrix" not in side_type:
            continue
        # 存在 Display Matrix 时，仅当可确认恒等才放行；未知字段 → 保守禁用 ROI
        if _is_identity_display_matrix(item):
            continue
        return False, f"non-identity display matrix: {item.get('side_data_type')}"

    return True, None


def _is_identity_display_matrix(side_data: dict[str, Any]) -> bool:
    """尽力识别恒等 Display Matrix；无法确认时返回 False。

    ffprobe 常见恒等矩阵为 16.16 定点，且 a33 = 2^30（不是 2^16）。
    若侧数据带 ``rotation`` 且 |rotation|≈0，优先信任该字段。
    """
    # ffprobe 常见：rotation 字段（度）— 优先信任
    rotation = side_data.get("rotation")
    if rotation is not None:
        try:
            return abs(float(rotation)) <= 1e-3
        except (TypeError, ValueError):
            return False

    # 部分版本给出 9 元矩阵字符串/列表（无 rotation 时才解析）
    matrix = side_data.get("displaymatrix") or side_data.get("matrix")
    if matrix is None:
        # 仅有 type、无 rotation/matrix：无法确认 → 非恒等
        return False

    values: list[float] = []
    if isinstance(matrix, str):
        parts = matrix.replace("\n", " ").split()
        for p in parts:
            try:
                values.append(float(p))
            except ValueError:
                continue
    elif isinstance(matrix, list):
        for p in matrix:
            try:
                values.append(float(p))
            except (TypeError, ValueError):
                return False
    else:
        return False

    if len(values) < 9:
        return False

    # 浮点恒等 3x3（行主序）
    float_id = [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0]
    if all(abs(values[i] - float_id[i]) <= 1e-3 for i in range(9)):
        return True

    # 16.16 定点恒等：a11=a22=65536，a33=2^30，其余 0
    fixed_id = [65536.0, 0.0, 0.0, 0.0, 65536.0, 0.0, 0.0, 0.0, 1073741824.0]
    max_abs = max(abs(v) for v in values[:9])
    if max_abs > 16.0:
        return all(abs(values[i] - fixed_id[i]) <= 1.0 for i in range(9))
    return False


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
