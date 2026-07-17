"""Benchmark 编排：视频 + ground truth → pipeline → 报告。

``run_benchmark`` 是一键入口：用现有 ``sublift`` 包跑端到端提取，与 ground
truth 一起交给诊断报告层分析。报告输出由 ``benchmark.report`` 负责。
"""

from __future__ import annotations

import hashlib
import multiprocessing as mp
import time
from contextlib import suppress
from dataclasses import dataclass, field, replace
from pathlib import Path
from queue import Empty
from typing import Any, Literal

from benchmark.diagnostics import TEXT_EMPTY, TEXT_NOISE, analyze_result
from benchmark.srt_loader import SrtEntry, load_srt
from sublift.config import Config
from sublift.detector.base import Detector
from sublift.diagnostics.performance import (
    PerformanceMode,
    PerformanceRecorder,
    aggregate_run_payloads,
    collect_environment,
    parse_performance_mode,
)
from sublift.extractor import FfmpegExtractor
from sublift.extractor.frame_io import plan_frame_io
from sublift.models import BoundingBox, SubtitleEntry
from sublift.ocr import MockOcrEngine, VisionOcrEngine, is_vision_available
from sublift.pipeline import Pipeline

# feat-037 固定 GT 质量门（与 phase3-opt-perf / feat-034 锚点对齐）
_QUALITY_TIMING_F1 = 0.952
_QUALITY_TIMING_PRECISION = 0.988
_QUALITY_USABLE = 0.851
_QUALITY_CER_MACRO = 0.066
_QUALITY_NOISE_MAX = 2
_QUALITY_EMPTY_MAX = 1

# 子进程结果读取：基础超时 + 按视频时长放大
_WORKER_BASE_TIMEOUT_S = 120.0
_WORKER_TIMEOUT_PER_VIDEO_S = 10.0
_WORKER_JOIN_TIMEOUT_S = 30.0


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
    subtitle_script: str = "auto"
    match_threshold: float = 0.5
    region_box: tuple[int, int, int, int] | None = None
    label: str | None = None
    video_duration_seconds: float | None = None
    output_dir: Path = field(default_factory=lambda: Path("debug/benchmark-reports"))
    # feat-037：开发者性能模式（默认 off，不改变旧行为）
    performance_mode: str = "off"
    warmup_runs: int = 0
    measured_runs: int = 1
    # 多 run 时默认独立进程隔离 peak RSS；单测可关以加速
    isolate_processes: bool = True
    # feat-038：内部 full/roi 输出对照；默认 full 保持历史路径可比
    # "full" = 全帧 RGB + FixedRegion(source)；"roi" = ffmpeg crop + RoiPassthrough
    frame_output_mode: str = "full"

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
    performance: dict[str, Any] | None = None


def run_benchmark(config: RunConfig) -> RunResult:
    """执行端到端 benchmark（支持 warmup + 多次 measured 性能聚合）。

    Args:
        config: 运行配置（视频、ground truth、采样率等）。

    Returns:
        ``RunResult``，含检测条目、ground truth 与运行耗时。
        若 ``performance_mode != off``，附带 ``performance`` 联合报告块。

    Raises:
        FileNotFoundError: 视频或 ground truth 文件不存在。
        RuntimeError: Vision 引擎不可用。
        ValueError: 非法 performance 配置。
    """
    if not config.video_path.exists():
        raise FileNotFoundError(f"视频文件不存在: {config.video_path}")
    if not config.ground_truth_path.exists():
        raise FileNotFoundError(f"ground truth 文件不存在: {config.ground_truth_path}")
    if config.warmup_runs < 0:
        raise ValueError("warmup_runs 不能为负")
    if config.measured_runs < 1:
        raise ValueError("measured_runs 必须 >= 1")
    _validate_frame_output_mode(config)

    mode = parse_performance_mode(config.performance_mode)
    ground_truth = load_srt(config.ground_truth_path)
    video_duration = config.video_duration_seconds or _probe_duration(config.video_path)
    multi = config.warmup_runs > 0 or config.measured_runs > 1
    isolate = bool(config.isolate_processes and multi)

    # 预热：丢弃结果（仍跑完整路径，避免冷启动污染 measured）
    for _ in range(config.warmup_runs):
        _execute_run(
            config,
            mode=PerformanceMode.OFF,
            video_duration=video_duration,
            isolate=isolate,
            segment_path=None,
        )

    measured: list[tuple[RunResult, dict[str, Any] | None]] = []
    for run_idx in range(config.measured_runs):
        segment_path = None
        if mode is PerformanceMode.TRACE:
            segment_path = (
                config.output_dir
                / f"{config.output_prefix}.perf-segments"
                / f"run{run_idx + 1}.jsonl"
            )
        single = _execute_run(
            config,
            mode=mode,
            video_duration=video_duration,
            isolate=isolate,
            segment_path=segment_path,
        )
        measured.append(single)

    # 每次 measured 均做质量快照；主报告仍用最后一次检测结果
    last_result, last_perf = measured[-1]
    quality_runs = [
        _quality_snapshot(result, run_index=i + 1)
        for i, (result, _perf) in enumerate(measured)
    ]
    hashes = [q["detection_hash"] for q in quality_runs]
    detections_consistent = len(set(hashes)) == 1
    all_quality_pass = all(bool(q["all_pass"]) for q in quality_runs)

    performance_payload: dict[str, Any] | None = None
    if mode is not PerformanceMode.OFF:
        run_payloads: list[dict[str, Any]] = []
        for (_result, perf), quality in zip(measured, quality_runs, strict=True):
            if perf is None:
                continue
            enriched = dict(perf)
            enriched["quality"] = quality
            run_payloads.append(enriched)
        performance_payload = {
            "schema_version": 1,
            "mode": mode.value,
            "completed": all(bool(p.get("completed", True)) for p in run_payloads),
            "environment": run_payloads[-1].get("environment", {}) if run_payloads else {},
            "workload": run_payloads[-1].get("workload", {}) if run_payloads else {},
            "latency": run_payloads[-1].get("latency", {}) if run_payloads else {},
            "throughput": run_payloads[-1].get("throughput", {}) if run_payloads else {},
            "resources": run_payloads[-1].get("resources", {}) if run_payloads else {},
            "stages": run_payloads[-1].get("stages", {}) if run_payloads else {},
            "counters": run_payloads[-1].get("counters", {}) if run_payloads else {},
            "runs": run_payloads,
            "aggregate": aggregate_run_payloads(run_payloads, primary="median"),
            "warmup_runs": config.warmup_runs,
            "measured_runs": config.measured_runs,
            "quality": {
                "runs": quality_runs,
                "detections_consistent": detections_consistent,
                "all_runs_pass": all_quality_pass,
                "reference_detection_hash": hashes[-1] if hashes else None,
            },
        }
        if mode is PerformanceMode.TRACE and last_perf is not None:
            if "segment_trace_path" in last_perf:
                performance_payload["segment_trace_path"] = last_perf["segment_trace_path"]
            performance_payload["segment_count"] = last_perf.get("segment_count")
    elif config.measured_runs > 1:
        # off 多跑时仍附带质量一致性信息，便于扰动对比
        last_result = replace(
            last_result,
            performance={
                "schema_version": 1,
                "mode": "off",
                "quality": {
                    "runs": quality_runs,
                    "detections_consistent": detections_consistent,
                    "all_runs_pass": all_quality_pass,
                    "reference_detection_hash": hashes[-1] if hashes else None,
                },
            },
        )
        return replace(last_result, ground_truth=ground_truth)

    return replace(
        last_result,
        ground_truth=ground_truth,
        performance=performance_payload,
    )


def _execute_run(
    config: RunConfig,
    *,
    mode: PerformanceMode,
    video_duration: float,
    isolate: bool,
    segment_path: Path | None,
) -> tuple[RunResult, dict[str, Any] | None]:
    """执行单次 run；可选 spawn 独立进程以隔离 peak RSS。"""
    if not isolate:
        return _run_once(
            config,
            mode=mode,
            video_duration=video_duration,
            segment_path=segment_path,
        )
    return _run_once_in_process(
        config,
        mode=mode,
        video_duration=video_duration,
        segment_path=segment_path,
    )


def _run_once_in_process(
    config: RunConfig,
    *,
    mode: PerformanceMode,
    video_duration: float,
    segment_path: Path | None,
) -> tuple[RunResult, dict[str, Any] | None]:
    """在 spawn 子进程中执行单次 run 并取回结果。

    先带超时 ``queue.get`` 再 ``join``，避免大结果撑满管道导致父子互锁。
    """
    ctx = mp.get_context("spawn")
    queue: mp.Queue[dict[str, Any]] = ctx.Queue()
    proc = ctx.Process(
        target=_isolated_worker,
        args=(
            queue,
            config,
            mode.value,
            video_duration,
            str(segment_path) if segment_path is not None else None,
        ),
    )
    timeout_s = _WORKER_BASE_TIMEOUT_S + max(0.0, video_duration) * _WORKER_TIMEOUT_PER_VIDEO_S
    proc.start()
    payload: dict[str, Any] | None = None
    try:
        try:
            payload = queue.get(timeout=timeout_s)
        except Empty as exc:
            _terminate_process(proc)
            raise RuntimeError(
                f"性能测量子进程结果超时（>{timeout_s:.0f}s）"
            ) from exc
    finally:
        if proc.is_alive():
            proc.join(timeout=_WORKER_JOIN_TIMEOUT_S)
        if proc.is_alive():
            _terminate_process(proc)

    if payload is None:
        raise RuntimeError("性能测量子进程未返回结果")
    if proc.exitcode not in (0, None) and payload.get("error"):
        raise RuntimeError(str(payload["error"]))
    if payload.get("error"):
        raise RuntimeError(str(payload["error"]))
    if proc.exitcode not in (0, None):
        raise RuntimeError(f"性能测量子进程异常退出: exitcode={proc.exitcode}")

    detected = [
        SrtEntry(
            index=int(item["index"]),
            start_ms=int(item["start_ms"]),
            end_ms=int(item["end_ms"]),
            text=str(item["text"]),
        )
        for item in payload["detected"]
    ]
    ground_truth = load_srt(config.ground_truth_path)
    result = RunResult(
        config=config,
        detected=detected,
        ground_truth=ground_truth,
        elapsed_seconds=float(payload["elapsed_seconds"]),
        video_duration_seconds=float(payload["video_duration_seconds"]),
        performance=payload.get("performance"),
    )
    return result, payload.get("performance")


def _terminate_process(proc: Any) -> None:
    """尽力终止并回收子进程。"""
    if not proc.is_alive():
        return
    with suppress(Exception):
        proc.terminate()
    proc.join(timeout=5.0)
    if proc.is_alive():
        with suppress(Exception):
            proc.kill()
        proc.join(timeout=5.0)


def _isolated_worker(
    queue: mp.Queue[dict[str, Any]],
    config: RunConfig,
    mode_value: str,
    video_duration: float,
    segment_path_str: str | None,
) -> None:
    """multiprocessing worker：必须为模块级函数以便 spawn pickle。"""
    try:
        mode = parse_performance_mode(mode_value)
        segment_path = Path(segment_path_str) if segment_path_str else None
        result, perf = _run_once(
            config,
            mode=mode,
            video_duration=video_duration,
            segment_path=segment_path,
        )
        queue.put(
            {
                "error": None,
                "detected": [
                    {
                        "index": e.index,
                        "start_ms": e.start_ms,
                        "end_ms": e.end_ms,
                        "text": e.text,
                    }
                    for e in result.detected
                ],
                "elapsed_seconds": result.elapsed_seconds,
                "video_duration_seconds": result.video_duration_seconds,
                "performance": perf,
            }
        )
    except Exception as exc:
        # 子进程必须回传错误字符串，不能让异常直接冲掉 queue
        queue.put({"error": f"{type(exc).__name__}: {exc}"})


def _run_once(
    config: RunConfig,
    *,
    mode: PerformanceMode,
    video_duration: float,
    segment_path: Path | None = None,
) -> tuple[RunResult, dict[str, Any] | None]:
    """执行单次 pipeline 提取；返回 (RunResult, performance payload|None)。"""
    recorder: PerformanceRecorder | None = None
    if mode is not PerformanceMode.OFF:
        recorder = PerformanceRecorder(mode=mode, segment_path=segment_path)
        recorder.set_environment(collect_environment())
        recorder.set_workload(
            {
                "video": str(config.video_path),
                "video_stem": config.video_path.stem,
                "ground_truth": str(config.ground_truth_path),
                "fps": config.fps,
                "engine": config.engine,
                "confidence": config.confidence,
                "subtitle_script": config.subtitle_script,
                "region_box": list(config.region_box) if config.region_box else None,
                "frame_output_mode": config.frame_output_mode,
                "video_duration_seconds": video_duration,
                "match_threshold": config.match_threshold,
            }
        )

    try:
        ocr = _build_ocr_engine(config.engine)
        extractor, detector = _build_extractor_and_detector(config, recorder)
        subtitle_profile = None
        if config.region_box is not None:
            from sublift.models import SubtitleProfile

            _x, _y, rw, rh = config.region_box
            subtitle_profile = SubtitleProfile.from_crop(
                rw,
                rh,
                script=config.subtitle_script,
            )
        pipeline_config = Config(
            sample_fps=config.fps,
            confidence_threshold=config.confidence,
            subtitle_profile=subtitle_profile,
            subtitle_script=config.subtitle_script,
        )
        pipeline = Pipeline(
            detector=detector,
            ocr=ocr,
            config=pipeline_config,
            extractor=extractor,
            performance_recorder=recorder,
        )

        ground_truth = load_srt(config.ground_truth_path)
        start = time.perf_counter()
        entries = pipeline.run(config.video_path)
        elapsed = time.perf_counter() - start
        detected = [_entry_to_srt(i, e) for i, e in enumerate(entries, start=1)]

        perf_payload = None
        if recorder is not None:
            recorder.sample_resources()
            perf_payload = recorder.to_payload()

        result = RunResult(
            config=config,
            detected=detected,
            ground_truth=ground_truth,
            elapsed_seconds=elapsed,
            video_duration_seconds=video_duration,
            performance=perf_payload,
        )
        return result, perf_payload
    except Exception:
        if recorder is not None:
            recorder.note_incomplete("exception")
            recorder.sample_resources()
        raise
    finally:
        if recorder is not None:
            recorder.close()


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


def _validate_frame_output_mode(config: RunConfig) -> None:
    """校验 frame_output_mode 与 region_box 组合（与 manifest 规则对齐）。"""
    mode = config.frame_output_mode
    if mode not in ("full", "roi"):
        raise ValueError(
            f"frame_output_mode 必须是 'full' 或 'roi'（收到 {mode!r}）"
        )
    if mode == "roi" and config.region_box is None:
        raise ValueError("frame_output_mode=roi 需要 region_box")


def _build_extractor_and_detector(
    config: RunConfig,
    recorder: PerformanceRecorder | None,
) -> tuple[FfmpegExtractor, Detector]:
    """经 plan_frame_io 一次定案 extractor + detector。"""
    region = None
    if config.region_box is not None:
        x, y, width, height = config.region_box
        region = BoundingBox(x=x, y=y, width=width, height=height)
    # benchmark 显式 full|roi；roi 下变换未验证硬失败
    plan_mode: Literal["full", "roi"] = (
        "roi" if config.frame_output_mode == "roi" else "full"
    )
    plan = plan_frame_io(
        config.video_path,
        region,
        mode=plan_mode,
        on_unvalidated_transform="error",
    )
    extractor = FfmpegExtractor(
        fps=config.fps,
        output_crop=plan.output_crop,
        source_info=plan.source,
        performance_recorder=recorder,
    )
    return extractor, plan.detector


def _entry_to_srt(index: int, entry: SubtitleEntry) -> SrtEntry:
    """将 ``SubtitleEntry`` 转换为 benchmark 内部 ``SrtEntry``。"""
    return SrtEntry(
        index=index,
        start_ms=entry.start_ms,
        end_ms=entry.end_ms,
        text=entry.text,
    )


def _detection_hash(detected: list[SrtEntry]) -> str:
    """稳定哈希：start/end/text，用于跨 measured run 比对检测一致性。"""
    digest = hashlib.sha256()
    for entry in detected:
        line = f"{entry.start_ms}\t{entry.end_ms}\t{entry.text}\n"
        digest.update(line.encode("utf-8"))
    return digest.hexdigest()[:16]


def _quality_snapshot(result: RunResult, *, run_index: int) -> dict[str, Any]:
    """对单次 measured run 计算质量门与 detection hash。"""
    analysis = analyze_result(result)
    timing = analysis.metrics.timing
    recognition = analysis.metrics.recognition
    e2e = analysis.metrics.e2e
    noise = sum(1 for case in analysis.gt_cases if case.classification == TEXT_NOISE)
    empty = sum(1 for case in analysis.gt_cases if case.classification == TEXT_EMPTY)

    checks: dict[str, dict[str, Any]] = {
        "timing_f1": {
            "actual": timing.timing_f1,
            "gate": _QUALITY_TIMING_F1,
            "pass": timing.timing_f1 >= _QUALITY_TIMING_F1,
        },
        "timing_precision": {
            "actual": timing.timing_precision,
            "gate": _QUALITY_TIMING_PRECISION,
            "pass": timing.timing_precision >= _QUALITY_TIMING_PRECISION,
        },
        "usable_subtitle_recall": {
            "actual": e2e.usable_subtitle_recall,
            "gate": _QUALITY_USABLE,
            "pass": e2e.usable_subtitle_recall >= _QUALITY_USABLE,
        },
        "cer_macro": {
            "actual": recognition.cer_macro,
            "gate": _QUALITY_CER_MACRO,
            "pass": recognition.cer_macro <= _QUALITY_CER_MACRO,
        },
        "text_noise": {
            "actual": noise,
            "gate": _QUALITY_NOISE_MAX,
            "pass": noise <= _QUALITY_NOISE_MAX,
        },
        "text_empty": {
            "actual": empty,
            "gate": _QUALITY_EMPTY_MAX,
            "pass": empty <= _QUALITY_EMPTY_MAX,
        },
    }
    return {
        "run_index": run_index,
        "detection_hash": _detection_hash(result.detected),
        "detected_count": len(result.detected),
        "elapsed_seconds": result.elapsed_seconds,
        "checks": checks,
        "all_pass": all(bool(item["pass"]) for item in checks.values()),
        "metrics": {
            "timing_f1": timing.timing_f1,
            "timing_precision": timing.timing_precision,
            "usable_subtitle_recall": e2e.usable_subtitle_recall,
            "cer_macro": recognition.cer_macro,
            "text_noise": noise,
            "text_empty": empty,
        },
    }


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
