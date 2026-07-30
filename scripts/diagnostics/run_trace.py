"""打轴决策 trace 与 FN 归因脚本。

对指定视频跑 pipeline + trace，输出逐帧决策 JSONL 与 FN 归因 Markdown
报告，用于定位打轴漏检（FN）的根因。

Usage:
    # 基础用法：跑 trace + FN 归因
    uv run python scripts/diagnostics/run_trace.py \\
        --video debug/Zootopia_clip_1080p.mp4 \\
        --ground-truth benchmark/datasets/Zootopia_clip_1080p_gt.srt \\
        --fps 5

    # 指定输出前缀与区域
    uv run python scripts/diagnostics/run_trace.py \\
        --video debug/Zootopia_clip_1080p.mp4 \\
        --ground-truth benchmark/datasets/Zootopia_clip_1080p_gt.srt \\
        --region-box 0 860 1920 220 \\
        --output-prefix debug/reports/feat031_region

输出:
    {prefix}_trace.jsonl   逐帧决策记录
    {prefix}_fn_analysis.md FN 归因报告
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parents[2]
if str(_REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(_REPO_ROOT))

from sublift.benchmark.diagnostics import TimingMetrics, analyze_entries  # noqa: E402
from sublift.benchmark.srt import SrtEntry, load_srt  # noqa: E402
from sublift.config import DEFAULT_CONFIG, ChangePointConfig, Config  # noqa: E402
from sublift.detector import BottomCropDetector, FixedRegionDetector  # noqa: E402
from sublift.diagnostics.fn_analysis import (  # noqa: E402
    DetectedSegment,
    FnClassification,
    classify_fn,
    format_fn_report,
)
from sublift.diagnostics.short_subtitle import (  # noqa: E402
    ShortSubtitleMetrics,
    compute_short_subtitle_metrics,
)
from sublift.diagnostics.trace import TraceRecorder  # noqa: E402
from sublift.extractor import FfmpegExtractor  # noqa: E402
from sublift.models import BoundingBox  # noqa: E402
from sublift.ocr import MockOcrEngine  # noqa: E402
from sublift.pipeline import Pipeline  # noqa: E402


def _reconstruct_segments_from_trace(
    recorder: TraceRecorder,
) -> list[DetectedSegment]:
    """从 trace 记录重建打轴段（绕过 OCR + dedupe）。

    trace 记录了每帧的事件类型（IN/OUT/CHANGE/None）。按事件流重建段：
    - IN: 开新段（start_ms = timestamp_ms）
    - OUT: 关当前段（end_ms = timestamp_ms）
    - CHANGE: 关当前段（end_ms = timestamp_ms）+ 开新段（start_ms = timestamp_ms）
    - 末尾未关闭段：用最后一条 trace 记录的 timestamp_ms 关闭

    Returns:
        重建的 DetectedSegment 列表。
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
                segments.append(DetectedSegment(start_ms=current_start, end_ms=rec.timestamp_ms))
                current_start = None
        elif event == "CHANGE":
            if current_start is not None:
                segments.append(DetectedSegment(start_ms=current_start, end_ms=rec.timestamp_ms))
            current_start = rec.timestamp_ms

    if current_start is not None:
        segments.append(DetectedSegment(start_ms=current_start, end_ms=last_ts))

    return segments


def build_parser() -> argparse.ArgumentParser:
    """Build the script argument parser."""
    parser = argparse.ArgumentParser(
        description="打轴决策 trace 与 FN 归因（feat-031a）。",
    )
    parser.add_argument("--video", type=Path, required=True, help="输入视频路径")
    parser.add_argument(
        "--ground-truth",
        type=Path,
        required=True,
        help="ground truth SRT 文件路径",
    )
    parser.add_argument("--fps", type=float, default=5.0, help="帧采样率（默认 5.0）")
    parser.add_argument(
        "--region-box",
        type=int,
        nargs=4,
        metavar=("X", "Y", "W", "H"),
        default=None,
        help="字幕区域 [x y width height]，不指定时用下部 30%% 裁剪",
    )
    parser.add_argument(
        "--enable-patrol",
        action="store_true",
        help="启用 SSIM 巡逻（feat-031b），补强 dHash 漏检的 CHANGE",
    )
    parser.add_argument(
        "--patrol-threshold",
        type=float,
        default=0.92,
        help="SSIM 巡逻阈值（默认 0.92，低于此值视为结构变化）",
    )
    parser.add_argument(
        "--patrol-interval",
        type=int,
        default=3,
        help="SSIM 巡逻间隔帧数（默认 3）",
    )
    parser.add_argument(
        "--match-threshold",
        type=float,
        default=0.5,
        help="segment 命中阈值（默认 0.5，与 benchmark 对齐）",
    )
    parser.add_argument(
        "--output-prefix",
        type=str,
        default=None,
        help="输出文件名前缀（默认 debug/reports/{video_stem}）",
    )
    parser.add_argument(
        "--jsonl",
        action="store_true",
        help="输出逐帧 JSONL（默认只输出 FN 归因 MD）",
    )
    parser.add_argument(
        "--compare-baseline",
        action="store_true",
        help="对比模式：跑 baseline（默认配置）与 optimized（patrol）两次，"
        "输出对比报告到 debug/reports/feat031_comparison.md",
    )
    return parser


def _run_pipeline_with_trace(
    video_path: Path,
    fps: float,
    region_box: tuple[int, int, int, int] | None,
    cp_config: ChangePointConfig,
) -> tuple[list[DetectedSegment], TraceRecorder]:
    """跑一次 pipeline + trace，返回重建的打轴段与 recorder。"""
    recorder = TraceRecorder()
    config = Config(
        sample_fps=fps,
        confidence_threshold=DEFAULT_CONFIG.confidence_threshold,
        change_point=cp_config,
    )
    extractor = FfmpegExtractor(fps=fps)
    detector = (
        FixedRegionDetector(
            BoundingBox(
                x=region_box[0],
                y=region_box[1],
                width=region_box[2],
                height=region_box[3],
            )
        )
        if region_box is not None
        else BottomCropDetector(bottom_ratio=config.region_bottom_ratio)
    )
    ocr = MockOcrEngine(text="[mock]", confidence=1.0)
    pipeline = Pipeline(
        detector=detector,
        ocr=ocr,
        config=config,
        extractor=extractor,
        trace_recorder=recorder,
    )
    pipeline.run(video_path)
    recorder.flush()
    detected = _reconstruct_segments_from_trace(recorder)
    return detected, recorder


def main(argv: list[str] | None = None) -> int:
    """Run trace pipeline and output FN analysis report."""
    parser = build_parser()
    args = parser.parse_args(argv)

    video_path: Path = args.video
    gt_path: Path = args.ground_truth
    if not video_path.exists():
        print(f"视频文件不存在: {video_path}", file=sys.stderr)
        return 1
    if not gt_path.exists():
        print(f"ground truth 文件不存在: {gt_path}", file=sys.stderr)
        return 1

    prefix = args.output_prefix or f"debug/reports/{video_path.stem}"
    prefix_path = Path(prefix)

    if args.compare_baseline:
        return _run_comparison(args, video_path, gt_path)

    # trace recorder
    jsonl_path = prefix_path.parent / f"{prefix_path.name}_trace.jsonl" if args.jsonl else None
    if jsonl_path is not None and jsonl_path.exists():
        jsonl_path.unlink()
    recorder = TraceRecorder(jsonl_path=jsonl_path)

    # build pipeline（用 mock OCR：trace 只关注打轴，不关心 OCR 文本）
    cp_config = ChangePointConfig()
    if args.enable_patrol:
        cp_config = ChangePointConfig(
            enable_ssim_patrol=True,
            ssim_patrol_interval=args.patrol_interval,
            ssim_patrol_threshold=args.patrol_threshold,
        )
    config = Config(
        sample_fps=args.fps,
        confidence_threshold=DEFAULT_CONFIG.confidence_threshold,
        change_point=cp_config,
    )
    extractor = FfmpegExtractor(fps=args.fps)
    detector = (
        FixedRegionDetector(
            BoundingBox(
                x=args.region_box[0],
                y=args.region_box[1],
                width=args.region_box[2],
                height=args.region_box[3],
            )
        )
        if args.region_box is not None
        else BottomCropDetector(bottom_ratio=config.region_bottom_ratio)
    )
    ocr = MockOcrEngine(text="[mock]", confidence=1.0)
    pipeline = Pipeline(
        detector=detector,
        ocr=ocr,
        config=config,
        extractor=extractor,
        trace_recorder=recorder,
    )

    print(f"运行 pipeline: {video_path} (fps={args.fps})...", file=sys.stderr)
    entries = pipeline.run(video_path)
    recorder.flush()
    print(f"完成：{len(entries)} 条字幕，{len(recorder.records)} 帧 trace", file=sys.stderr)

    # FN 归因
    # 注意：trace 用 mock OCR，所有段文本相同会被 dedupe 合并。
    # 为了看打轴段（而非合并后段），从 trace 的事件记录重建检测段。
    detected = _reconstruct_segments_from_trace(recorder)
    if not detected:
        # fallback：用 pipeline 输出
        detected = [
            DetectedSegment(start_ms=e.start_ms, end_ms=e.end_ms, text=e.text) for e in entries
        ]
    ground_truth = load_srt(gt_path)
    classifications = classify_fn(detected, ground_truth, match_threshold=args.match_threshold)

    fn_md_path = prefix_path.parent / f"{prefix_path.name}_fn_analysis.md"
    report = format_fn_report(classifications, output_path=fn_md_path)

    print(f"\nFN 归因报告: {fn_md_path}", file=sys.stderr)
    if jsonl_path is not None:
        print(f"逐帧 trace: {jsonl_path}", file=sys.stderr)
    print(file=sys.stderr)
    print(report)
    return 0


def _run_comparison(args: argparse.Namespace, video_path: Path, gt_path: Path) -> int:
    """对比模式：baseline（默认配置） vs optimized（patrol）。"""
    region_box = tuple(args.region_box) if args.region_box else None

    ground_truth = load_srt(gt_path)

    print("运行 baseline（默认配置）...", file=sys.stderr)
    base_detected, _ = _run_pipeline_with_trace(
        video_path, args.fps, region_box, ChangePointConfig()
    )
    base_seg = _compute_segment_metrics(base_detected, ground_truth, args.match_threshold)
    base_fn = classify_fn(base_detected, ground_truth, match_threshold=args.match_threshold)
    base_short = compute_short_subtitle_metrics(base_detected, ground_truth)
    print(
        f"  F1={base_seg.timing_f1 * 100:.1f}% FN={len(base_fn)}"
        f" short={base_short.short_recall * 100:.1f}%",
        file=sys.stderr,
    )

    patrol_config = ChangePointConfig(
        enable_ssim_patrol=True,
        ssim_patrol_interval=args.patrol_interval,
        ssim_patrol_threshold=args.patrol_threshold,
    )
    print("运行 optimized（patrol 启用）...", file=sys.stderr)
    opt_detected, _ = _run_pipeline_with_trace(video_path, args.fps, region_box, patrol_config)
    opt_seg = _compute_segment_metrics(opt_detected, ground_truth, args.match_threshold)
    opt_fn = classify_fn(opt_detected, ground_truth, match_threshold=args.match_threshold)
    opt_short = compute_short_subtitle_metrics(opt_detected, ground_truth)
    print(
        f"  F1={opt_seg.timing_f1 * 100:.1f}% FN={len(opt_fn)}"
        f" short={opt_short.short_recall * 100:.1f}%",
        file=sys.stderr,
    )

    report = _format_comparison(base_seg, opt_seg, base_fn, opt_fn, base_short, opt_short)
    out_path = Path("debug/reports/feat031_comparison.md")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(report, encoding="utf-8")
    print(f"\n对比报告: {out_path}", file=sys.stderr)
    print(report)
    return 0


def _compute_segment_metrics(
    detected: list[DetectedSegment],
    ground_truth: list[SrtEntry],
    match_threshold: float,
) -> TimingMetrics:
    """计算 timing F1（复用 sublift.benchmark.diagnostics 一对一匹配）。"""
    detected_srt = [
        SrtEntry(index=i + 1, start_ms=d.start_ms, end_ms=d.end_ms, text=d.text)
        for i, d in enumerate(detected)
    ]
    analysis = analyze_entries(
        detected_srt,
        ground_truth,
        temporal_iou_threshold=match_threshold,
    )
    return analysis.metrics.timing


def _format_comparison(
    base_seg: TimingMetrics,
    opt_seg: TimingMetrics,
    base_fn: list[FnClassification],
    opt_fn: list[FnClassification],
    base_short: ShortSubtitleMetrics,
    opt_short: ShortSubtitleMetrics,
) -> str:
    """格式化对比报告 Markdown。"""
    from collections import Counter

    base_fn_types = Counter(c.fn_type for c in base_fn)
    opt_fn_types = Counter(c.fn_type for c in opt_fn)

    lines: list[str] = ["# feat-031 对比验证报告", ""]
    lines.append("## 配置")
    lines.append("- Baseline: 默认 ChangePointConfig（patrol 关闭）")
    lines.append("- Optimized: enable_ssim_patrol=True, interval=3, threshold=0.92")
    lines.append("")

    lines.append("## 指标对比")
    lines.append("")
    lines.append("| 指标 | Baseline | Optimized | 变化 |")
    lines.append("|---|---|---|---|")
    lines.append(
        f"| Timing F1 | {base_seg.timing_f1 * 100:.1f}% | {opt_seg.timing_f1 * 100:.1f}% | "
        f"{(opt_seg.timing_f1 - base_seg.timing_f1) * 100:+.1f}pp |"
    )
    lines.append(
        f"| Recall | {base_seg.timing_recall * 100:.1f}% | {opt_seg.timing_recall * 100:.1f}% | "
        f"{(opt_seg.timing_recall - base_seg.timing_recall) * 100:+.1f}pp |"
    )
    lines.append(
        f"| Precision | {base_seg.timing_precision * 100:.1f}% | "
        f"{opt_seg.timing_precision * 100:.1f}% | "
        f"{(opt_seg.timing_precision - base_seg.timing_precision) * 100:+.1f}pp |"
    )
    lines.append(f"| FN 总数 | {len(base_fn)} | {len(opt_fn)} | {len(opt_fn) - len(base_fn):+d} |")
    lines.append(
        f"| 短字幕召回率 | {base_short.short_recall * 100:.1f}% | "
        f"{opt_short.short_recall * 100:.1f}% | "
        f"{(opt_short.short_recall - base_short.short_recall) * 100:+.1f}pp |"
    )
    lines.append("")

    lines.append("## FN 类型分布")
    lines.append("")
    lines.append("| FN 类型 | Baseline | Optimized | 变化 |")
    lines.append("|---|---|---|---|")
    for fn_type in ("no_overlap", "merged_into_neighbor", "boundary_miss"):
        b = base_fn_types.get(fn_type, 0)
        o = opt_fn_types.get(fn_type, 0)
        lines.append(f"| {fn_type} | {b} | {o} | {o - b:+d} |")
    lines.append("")

    lines.append("## 改善场景记录")
    lines.append("")
    base_merged = [c for c in base_fn if c.fn_type == "merged_into_neighbor"]
    opt_merged = [c for c in opt_fn if c.fn_type == "merged_into_neighbor"]
    if len(base_merged) > len(opt_merged):
        lines.append(
            f"- 场景: 连续相似中文字幕合并漏检\n"
            f"  优化前: {len(base_merged)} 条 merged_into_neighbor\n"
            f"  优化后: {len(opt_merged)} 条 merged_into_neighbor\n"
            f"  改善: 减少 {len(base_merged) - len(opt_merged)} 条"
        )
    else:
        lines.append("- (无显著改善场景)")
    lines.append("")

    lines.append("## 参数落定决策")
    lines.append("")
    f1_delta = opt_seg.timing_f1 - base_seg.timing_f1
    precision_delta = opt_seg.timing_precision - base_seg.timing_precision
    if opt_seg.timing_f1 >= 0.95 and precision_delta >= 0:
        lines.append("- 决策: **参数落定为默认值**（F1 ≥ 95% 且 precision 不下降）")
    elif f1_delta >= 0.03 and precision_delta >= 0:
        lines.append(
            f"- 决策: **记为推荐配置**（F1 提升 {f1_delta * 100:+.1f}pp ≥ 3pp，"
            "precision 不下降，但未达 95%）"
        )
        lines.append("- 不修改 config.py 默认值，patrol 默认保持关闭")
    else:
        lines.append(f"- 决策: **仅记录**（F1 提升 {f1_delta * 100:+.1f}pp < 3pp，不落定参数）")
    lines.append("")

    return "\n".join(lines)


if __name__ == "__main__":
    raise SystemExit(main())
