"""短字幕参数扫描脚本（feat-031c）。

对 ``presence_threshold`` / ``hysteresis_frames`` / ``min_duration_ms`` 各
跑一组参数，输出整体 segment F1 + 短字幕召回率对比表，用于评估各参数
对短字幕（< 1.5s）的影响。

用法：
    uv run python scripts/scan_params.py \\
        --video debug/Zootopia_clip_1080p.mp4 \\
        --ground-truth benchmark/fixtures/Zootopia_clip_1080p_gt.srt \\
        --region-box 0 860 1920 220
"""

from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parents[1]
if str(_REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(_REPO_ROOT))

from benchmark.diagnostics import analyze_entries  # noqa: E402
from benchmark.srt_loader import SrtEntry, load_srt  # noqa: E402

from sublift.config import ChangePointConfig, Config  # noqa: E402
from sublift.detector import FixedRegionDetector  # noqa: E402
from sublift.diagnostics.fn_analysis import DetectedSegment  # noqa: E402
from sublift.diagnostics.short_subtitle import (  # noqa: E402
    ShortSubtitleMetrics,
    compute_short_subtitle_metrics,
    format_short_subtitle_report,
)
from sublift.diagnostics.trace import TraceRecorder  # noqa: E402


def _reconstruct_segments_from_trace(
    recorder: TraceRecorder,
) -> list[DetectedSegment]:
    """从 trace 记录重建打轴段（绕过 OCR + dedupe）。

    与 ``run_trace.py`` 中的实现一致：按事件流（IN/OUT/CHANGE）重建段。
    """
    segments: list[DetectedSegment] = []
    current_start: int | None = None
    last_ts = 0

    for rec in recorder.records:
        last_ts = rec.timestamp_ms
        event = rec.event_type
        if event == "IN":
            current_start = rec.timestamp_ms
        elif event == "OUT":
            if current_start is not None:
                segments.append(
                    DetectedSegment(start_ms=current_start, end_ms=rec.timestamp_ms)
                )
                current_start = None
        elif event == "CHANGE":
            if current_start is not None:
                segments.append(
                    DetectedSegment(start_ms=current_start, end_ms=rec.timestamp_ms)
                )
            current_start = rec.timestamp_ms

    if current_start is not None:
        segments.append(DetectedSegment(start_ms=current_start, end_ms=last_ts))

    return segments
from sublift.extractor import FfmpegExtractor  # noqa: E402
from sublift.models import BoundingBox  # noqa: E402
from sublift.ocr import MockOcrEngine  # noqa: E402
from sublift.pipeline import Pipeline  # noqa: E402


@dataclass(frozen=True)
class ScanResult:
    """单次参数扫描结果。"""

    label: str
    segment_f1: float
    segment_recall: float
    segment_precision: float
    short_metrics: ShortSubtitleMetrics


def build_parser() -> argparse.ArgumentParser:
    """Build the script argument parser."""
    parser = argparse.ArgumentParser(
        description="短字幕参数扫描（feat-031c）。",
    )
    parser.add_argument("--video", type=Path, required=True, help="输入视频路径")
    parser.add_argument(
        "--ground-truth", type=Path, required=True, help="ground truth SRT 路径"
    )
    parser.add_argument("--fps", type=float, default=5.0, help="帧采样率（默认 5.0）")
    parser.add_argument(
        "--region-box",
        type=int,
        nargs=4,
        metavar=("X", "Y", "W", "H"),
        required=True,
        help="字幕区域 [x y width height]",
    )
    parser.add_argument(
        "--short-threshold-ms",
        type=int,
        default=1500,
        help="短字幕时长阈值（默认 1500ms）",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("debug/reports/feat031_param_scan.md"),
        help="输出报告路径",
    )
    return parser


def _run_once(
    video: Path,
    ground_truth: list[SrtEntry],
    fps: float,
    region_box: tuple[int, int, int, int],
    cp_config: ChangePointConfig,
    min_duration_ms: int,
    short_threshold_ms: int,
    label: str,
) -> ScanResult:
    """跑一次 pipeline + 指标计算。"""
    recorder = TraceRecorder()
    config = Config(
        sample_fps=fps,
        min_duration_ms=min_duration_ms,
        change_point=cp_config,
    )
    extractor = FfmpegExtractor(fps=fps)
    detector = FixedRegionDetector(
        BoundingBox(
            x=region_box[0],
            y=region_box[1],
            width=region_box[2],
            height=region_box[3],
        )
    )
    ocr = MockOcrEngine(text="[mock]", confidence=1.0)
    pipeline = Pipeline(
        detector=detector,
        ocr=ocr,
        config=config,
        extractor=extractor,
        trace_recorder=recorder,
    )

    pipeline.run(video)
    recorder.flush()

    # 从 trace 事件重建打轴段（绕过 OCR + dedupe，与 run_trace 一致）
    detected = _reconstruct_segments_from_trace(recorder)
    detected_srt = [
        SrtEntry(index=i + 1, start_ms=d.start_ms, end_ms=d.end_ms, text=d.text)
        for i, d in enumerate(detected)
    ]
    timing = analyze_entries(detected_srt, ground_truth).metrics.timing
    short = compute_short_subtitle_metrics(
        detected, ground_truth, threshold_ms=short_threshold_ms
    )

    return ScanResult(
        label=label,
        segment_f1=timing.timing_f1,
        segment_recall=timing.timing_recall,
        segment_precision=timing.timing_precision,
        short_metrics=short,
    )


def _format_report(results: list[ScanResult]) -> str:
    """格式化扫描结果为 Markdown。"""
    lines: list[str] = ["# 短字幕参数扫描报告", ""]
    lines.append("## 整体指标 + 短字幕召回率")
    lines.append("")
    lines.append("| 配置 | F1 | Recall | Precision | 短字幕数 | 短字幕命中 | 短字幕召回率 |")
    lines.append("|---|---|---|---|---|---|---|")
    for r in results:
        lines.append(
            f"| {r.label} | {r.segment_f1 * 100:.1f}% | "
            f"{r.segment_recall * 100:.1f}% | "
            f"{r.segment_precision * 100:.1f}% | "
            f"{r.short_metrics.short_gt_count} | "
            f"{r.short_metrics.short_matched} | "
            f"{r.short_metrics.short_recall * 100:.1f}% |"
        )
    lines.append("")

    short_metrics_list = [r.short_metrics for r in results]
    labels = [r.label for r in results]
    lines.append(format_short_subtitle_report(short_metrics_list, labels=labels))

    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    """Run parameter scan."""
    parser = build_parser()
    args = parser.parse_args(argv)

    video: Path = args.video
    gt_path: Path = args.ground_truth
    if not video.exists():
        print(f"视频文件不存在: {video}", file=sys.stderr)
        return 1
    if not gt_path.exists():
        print(f"ground truth 文件不存在: {gt_path}", file=sys.stderr)
        return 1

    ground_truth = load_srt(gt_path)
    region_box = tuple(args.region_box)
    fps = args.fps
    short_thr = args.short_threshold_ms

    results: list[ScanResult] = []

    print("扫描 presence_threshold...", file=sys.stderr)
    for pt in (0.005, 0.01, 0.02, 0.03):
        cp = ChangePointConfig(presence_threshold=pt)
        r = _run_once(
            video, ground_truth, fps, region_box, cp, 500, short_thr,
            f"presence={pt}",
        )
        results.append(r)
        print(
            f"  {r.label}: F1={r.segment_f1:.3f}"
            f" short={r.short_metrics.short_recall:.3f}",
            file=sys.stderr,
        )

    print("扫描 hysteresis_frames...", file=sys.stderr)
    for hf in (1, 2, 3):
        cp = ChangePointConfig(hysteresis_frames=hf)
        r = _run_once(
            video, ground_truth, fps, region_box, cp, 500, short_thr,
            f"hysteresis={hf}",
        )
        results.append(r)
        print(
            f"  {r.label}: F1={r.segment_f1:.3f}"
            f" short={r.short_metrics.short_recall:.3f}",
            file=sys.stderr,
        )

    print("扫描 min_duration_ms...", file=sys.stderr)
    for md in (200, 300, 500, 800):
        cp = ChangePointConfig()
        r = _run_once(
            video, ground_truth, fps, region_box, cp, md, short_thr,
            f"min_duration={md}",
        )
        results.append(r)
        print(
            f"  {r.label}: F1={r.segment_f1:.3f}"
            f" short={r.short_metrics.short_recall:.3f}",
            file=sys.stderr,
        )

    report = _format_report(results)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(report, encoding="utf-8")
    print(f"\n报告已写入: {args.output}", file=sys.stderr)
    print(report)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
