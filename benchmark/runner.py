"""Benchmark 编排：视频 + ground truth → pipeline → 报告。

``run_benchmark`` 是一键入口：用现有 ``sublift`` 包跑端到端提取，与 ground
truth 一起交给诊断报告层分析。报告输出由 ``benchmark.report`` 负责。
"""

from __future__ import annotations

import time
from dataclasses import dataclass, field
from pathlib import Path

from benchmark.srt_loader import SrtEntry, load_srt
from sublift.config import DEFAULT_CONFIG, Config
from sublift.detector import BottomCropDetector, FixedRegionDetector
from sublift.extractor import FfmpegExtractor
from sublift.models import BoundingBox, SubtitleEntry
from sublift.ocr import MockOcrEngine, VisionOcrEngine, is_vision_available
from sublift.pipeline import Pipeline


@dataclass(frozen=True)
class RunConfig:
    """单次 benchmark 运行配置。

    与 ``sublift.cli`` 的 CLI 参数对齐，但作为 benchmark 的显式输入。
    """

    video_path: Path
    ground_truth_path: Path
    fps: float = 5.0
    engine: str = "vision"
    confidence: float = 0.5
    match_threshold: float = 0.5
    region_box: tuple[int, int, int, int] | None = None
    label: str | None = None
    video_duration_seconds: float | None = None
    output_dir: Path = field(default_factory=lambda: Path("debug/benchmark-reports"))

    @property
    def output_prefix(self) -> str:
        """生成的报告文件名前缀（视频名 stem + 可选 label 后缀）。"""
        base = self.video_path.stem
        return f"{base}_{self.label}" if self.label else base


@dataclass(frozen=True)
class RunResult:
    """一次 benchmark 运行的完整结果。"""

    config: RunConfig
    detected: list[SrtEntry]
    ground_truth: list[SrtEntry]
    elapsed_seconds: float
    video_duration_seconds: float
    exported_srt_path: Path | None = None


def run_benchmark(config: RunConfig) -> RunResult:
    """执行一次端到端 benchmark。

    Args:
        config: 运行配置（视频、ground truth、采样率等）。

    Returns:
        ``RunResult``，含检测条目、ground truth 与运行耗时。

    Raises:
        FileNotFoundError: 视频或 ground truth 文件不存在。
        RuntimeError: Vision 引擎不可用。
    """
    if not config.video_path.exists():
        raise FileNotFoundError(f"视频文件不存在: {config.video_path}")
    if not config.ground_truth_path.exists():
        raise FileNotFoundError(f"ground truth 文件不存在: {config.ground_truth_path}")

    ocr = _build_ocr_engine(config.engine)
    extractor = FfmpegExtractor(fps=config.fps)
    detector = _build_detector(config.region_box)
    subtitle_profile = None
    if config.region_box is not None:
        from sublift.models import SubtitleProfile

        _x, _y, rw, rh = config.region_box
        subtitle_profile = SubtitleProfile.from_crop(rw, rh)
    pipeline_config = Config(
        sample_fps=config.fps,
        confidence_threshold=config.confidence,
        subtitle_profile=subtitle_profile,
    )
    pipeline = Pipeline(detector=detector, ocr=ocr, config=pipeline_config, extractor=extractor)

    ground_truth = load_srt(config.ground_truth_path)
    video_duration = config.video_duration_seconds or _probe_duration(config.video_path)

    start = time.perf_counter()
    entries = pipeline.run(config.video_path)
    elapsed = time.perf_counter() - start

    detected = [_entry_to_srt(i, e) for i, e in enumerate(entries, start=1)]

    return RunResult(
        config=config,
        detected=detected,
        ground_truth=ground_truth,
        elapsed_seconds=elapsed,
        video_duration_seconds=video_duration,
    )


def align_existing_srt(
    config: RunConfig,
    detected_path: Path,
    *,
    elapsed_seconds: float | None = None,
    video_duration_seconds: float | None = None,
) -> RunResult:
    """对已存在的 SRT 产物计算指标（不跑 pipeline）。

    用于复现历史/外部跑出的结果，例如 GUI 端用选定区域导出的 SRT。

    Args:
        config: 运行配置（label 用于区分基线）。
        detected_path: 已存在的 SRT 文件路径。
        elapsed_seconds: 可选耗时；缺省时速度指标为 0。
        video_duration_seconds: 可选视频时长；缺省时从 config.video_path 探测。

    Returns:
        ``RunResult``。
    """
    if not detected_path.exists():
        raise FileNotFoundError(f"检测产物 SRT 不存在: {detected_path}")
    if not config.ground_truth_path.exists():
        raise FileNotFoundError(f"ground truth 文件不存在: {config.ground_truth_path}")

    ground_truth = load_srt(config.ground_truth_path)
    detected = load_srt(detected_path)

    dur = video_duration_seconds
    if dur is None and config.video_path.exists():
        dur = _probe_duration(config.video_path)
    dur = dur or 0.0
    elapsed = elapsed_seconds or 0.0
    return RunResult(
        config=config,
        detected=detected,
        ground_truth=ground_truth,
        elapsed_seconds=elapsed,
        video_duration_seconds=dur,
        exported_srt_path=detected_path,
    )


def _build_ocr_engine(engine: str) -> VisionOcrEngine | MockOcrEngine:
    """构造 OCR 引擎（与 cli._build_ocr_engine 对齐，独立实现避免循环依赖）。"""
    if engine == "vision":
        if not is_vision_available():
            raise RuntimeError(
                "Apple Vision 不可用，请执行 `uv sync --extra vision` 或改用 --engine mock"
            )
        return VisionOcrEngine()
    if engine == "mock":
        return MockOcrEngine(text="[mock subtitle]", confidence=1.0)
    raise ValueError(f"未知引擎: {engine}")


def _build_detector(
    region_box: tuple[int, int, int, int] | None,
) -> BottomCropDetector | FixedRegionDetector:
    """按 region_box 选择检测器；None 时回退下部裁剪（与 ipc.bridge 一致）。"""
    if region_box is not None:
        x, y, width, height = region_box
        return FixedRegionDetector(BoundingBox(x=x, y=y, width=width, height=height))
    return BottomCropDetector(bottom_ratio=DEFAULT_CONFIG.region_bottom_ratio)


def _entry_to_srt(index: int, entry: SubtitleEntry) -> SrtEntry:
    """将 ``SubtitleEntry`` 转换为 benchmark 内部 ``SrtEntry``。"""
    return SrtEntry(
        index=index,
        start_ms=entry.start_ms,
        end_ms=entry.end_ms,
        text=entry.text,
    )


def _probe_duration(video_path: Path) -> float:
    """用 ffprobe 探测视频时长（秒）。失败时回退到 ground truth 最后时间码。"""
    import json
    import subprocess

    try:
        result = subprocess.run(
            [
                "ffprobe",
                "-v",
                "error",
                "-show_entries",
                "format=duration",
                "-of",
                "json",
                str(video_path),
            ],
            capture_output=True,
            text=True,
            check=True,
        )
        data = json.loads(result.stdout)
        return float(data["format"]["duration"])
    except (subprocess.CalledProcessError, KeyError, ValueError, FileNotFoundError):
        return 0.0
