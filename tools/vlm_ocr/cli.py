"""CLI entrypoint orchestrating timing -> sheets -> VLM OCR -> assembly -> evaluation."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from typing import Any

from .assembler import assemble_cues_from_slots, write_srt
from .env_config import resolve_vlm_config
from .evaluator import evaluate_srt_pair, load_authoritative_srt_eval
from .models import (
    CropBox,
    FailedSlotRecord,
    PLACEHOLDER_CATEGORY,
    PLACEHOLDER_TEXT,
    SlotFallbackResolver,
    TimingSlot,
    TranscribedSlot,
)
from .sheet_builder import (
    InMemoryStripExtractor,
    compute_strip_crop_geometry,
    compute_target_strip_height,
)
from .timing_adapter import (
    _normalize_skip_frame,
    resolve_sublift_cli,
    stream_timing_stage,
)
from .vlm_client import summarize_vlm_usage, transcribe_streaming_sheets


VIDEO_EXTENSIONS = frozenset({".mp4", ".mkv", ".mov", ".avi", ".webm", ".ts", ".m4v"})


def _default_hwaccel() -> str:
    return "auto"


def _default_skip_frame() -> str:
    try:
        return _normalize_skip_frame(os.environ.get("SUBLIFT_FFMPEG_SKIP_FRAME", "auto"))
    except ValueError:
        return "auto"


def _discover_videos(input_path: Path, recursive: bool = False) -> list[Path]:
    if input_path.is_file():
        return [input_path]
    if not input_path.is_dir():
        raise FileNotFoundError(f"Input path does not exist: {input_path}")
    it = input_path.rglob("*") if recursive else input_path.iterdir()
    videos = sorted(
        (p for p in it if p.is_file() and p.suffix.lower() in VIDEO_EXTENSIONS),
        key=lambda p: str(p),
    )
    if not videos:
        raise FileNotFoundError(f"No video files found under directory: {input_path}")
    return videos


def _run_paddle_baseline(
    *,
    cli_bin: Path,
    video_path: Path,
    out_srt: Path,
    fps: float,
    region: CropBox | None,
    bottom_ratio: float,
    threads: int,
    script: str,
    hwaccel: str = "auto",
    skip_frame: str = "auto",
    quiet: bool,
) -> float:
    norm_skip = _normalize_skip_frame(skip_frame)
    cmd = [
        str(cli_bin),
        "-i",
        str(video_path),
        "-o",
        str(out_srt),
        "--fps",
        str(fps),
        "--script",
        script,
        "--bottom-ratio",
        str(bottom_ratio),
        "--hwaccel",
        hwaccel or "auto",
        "--skip-frame",
        norm_skip,
    ]
    if threads > 0:
        cmd.extend(["--threads", str(threads)])
    if region is not None:
        cmd.extend(["--region", region.to_cli_arg()])

    if not quiet:
        print(f"[Compare] Running local PaddleOCR baseline on {video_path.name}...", file=sys.stderr, flush=True)
    t0 = time.perf_counter()
    proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
    elapsed_s = time.perf_counter() - t0
    if proc.returncode != 0:
        raise RuntimeError(
            f"sublift_cli PaddleOCR baseline failed (exit={proc.returncode}):\n"
            f"{proc.stderr.strip() or proc.stdout.strip()}"
        )
    (out_srt.with_suffix(".paddle.log")).write_text(
        proc.stdout + ("\n--- STDERR ---\n" + proc.stderr if proc.stderr else ""),
        encoding="utf-8",
    )
    return round(elapsed_s, 3)


class PlaceholderSlotFallbackResolver:
    """Fallback resolver emitting `[待补录]` placeholder cues when a 1-slot leaf fails VLM OCR."""

    def resolve_failed_slot(
        self,
        slot: TimingSlot,
        strip_image: Any,
        reason: str,
    ) -> tuple[TranscribedSlot, FailedSlotRecord]:
        del strip_image
        t_slot = TranscribedSlot(
            slot_id=slot.slot_id,
            category=PLACEHOLDER_CATEGORY,
            lines=[PLACEHOLDER_TEXT],
        )
        rec = FailedSlotRecord(
            slot_id=slot.slot_id,
            start_ms=slot.start_ms,
            end_ms=slot.end_ms,
            reason=reason,
            resolution="placeholder",
            fallback_text=PLACEHOLDER_TEXT,
        )
        return t_slot, rec


class PaddleSlotFallbackResolver:
    """Fallback resolver invoking local PaddleOCR (`sublift_cli`) over an isolated failed slot."""

    def __init__(
        self,
        *,
        cli_bin: Path,
        video_path: Path,
        resolved_region: CropBox,
        work_dir: Path,
        fps: float,
        bottom_ratio: float,
        threads: int,
        script: str,
        hwaccel: str,
        repo_root: Path,
    ) -> None:
        self._cli_bin = cli_bin
        self._video_path = video_path
        self._region = resolved_region
        self._rescue_root = work_dir / "paddle_rescue"
        self._fps = fps
        self._bottom_ratio = bottom_ratio
        self._threads = threads
        self._script = script
        self._hwaccel = hwaccel
        self._repo_root = repo_root
        self._lock = threading.Lock()

    def resolve_failed_slot(
        self,
        slot: TimingSlot,
        strip_image: Any,
        reason: str,
    ) -> tuple[TranscribedSlot, FailedSlotRecord]:
        del strip_image
        start_ms = max(0, int(slot.start_ms) - 100)
        end_ms = max(start_ms + 1, int(slot.end_ms) + 100)
        self._rescue_root.mkdir(parents=True, exist_ok=True)

        lines: list[str] = []
        resolution = "paddle"
        with self._lock, tempfile.TemporaryDirectory(
            prefix=f"slot_{slot.slot_id:04d}_", dir=self._rescue_root
        ) as tmp_dir:
            out_srt = Path(tmp_dir) / "slot.srt"
            cmd = [
                str(self._cli_bin),
                "-i",
                str(self._video_path),
                "-o",
                str(out_srt),
                "--fps",
                str(self._fps),
                "--script",
                self._script,
                "--bottom-ratio",
                str(self._bottom_ratio),
                "--region",
                self._region.to_cli_arg(),
                "--start-ms",
                str(start_ms),
                "--end-ms",
                str(end_ms),
                "--hwaccel",
                self._hwaccel,
            ]
            if self._threads > 0:
                cmd.extend(["--threads", str(self._threads)])

            try:
                proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
                if proc.returncode == 0 and out_srt.is_file():
                    srt_eval = load_authoritative_srt_eval(self._repo_root)
                    cues = srt_eval.parse_srt(str(out_srt))
                    best_overlap = 0
                    best_lines: list[str] = []
                    for c_start, c_end, c_text in cues:
                        ov = max(0, min(c_end, slot.end_ms) - max(c_start, slot.start_ms))
                        cand = [ln.strip() for ln in str(c_text).splitlines() if ln.strip()]
                        if cand and ov > best_overlap:
                            best_overlap = ov
                            best_lines = cand
                    lines = best_lines
                else:
                    resolution = "placeholder_after_paddle_error"
            except Exception:
                resolution = "placeholder_after_paddle_error"

        if lines:
            text_joined = "\n".join(lines)
            return (
                TranscribedSlot(slot_id=slot.slot_id, category="dialogue", lines=lines),
                FailedSlotRecord(
                    slot_id=slot.slot_id,
                    start_ms=slot.start_ms,
                    end_ms=slot.end_ms,
                    reason=reason,
                    resolution=resolution,
                    fallback_text=text_joined,
                ),
            )

        fallback_res = (
            resolution if resolution != "paddle" else "placeholder_after_paddle_empty"
        )
        return (
            TranscribedSlot(
                slot_id=slot.slot_id,
                category=PLACEHOLDER_CATEGORY,
                lines=[PLACEHOLDER_TEXT],
            ),
            FailedSlotRecord(
                slot_id=slot.slot_id,
                start_ms=slot.start_ms,
                end_ms=slot.end_ms,
                reason=reason,
                resolution=fallback_res,
                fallback_text=PLACEHOLDER_TEXT,
            ),
        )


def process_single_video(
    video_path: Path,
    *,
    out_srt: Path,
    work_dir: Path,
    args: argparse.Namespace,
    repo_root: Path,
) -> dict[str, Any]:
    """Run streaming VLM OCR + optional Paddle comparison & GT evaluation for one video."""
    t_total0 = time.perf_counter()
    work_dir.mkdir(parents=True, exist_ok=True)
    reuse_cache = not args.no_cache
    quiet = bool(args.quiet)
    save_debug = bool(getattr(args, "save_debug_images", False) or getattr(args, "save_crops", False))

    cli_bin = resolve_sublift_cli(args.cli, repo_root=repo_root)
    explicit_region = CropBox.parse(args.region) if args.region else None
    strips_tmp_dir = (work_dir / ".strips_stream_tmp") if args.timing_input is None else None
    if strips_tmp_dir is not None:
        shutil.rmtree(strips_tmp_dir, ignore_errors=True)

    # Stage 1: Eager region header + live TimingSlot stream from sublift_cli --timing-only
    header, slot_stream, timing_meta = stream_timing_stage(
        video_path,
        work_dir,
        cli_bin=cli_bin,
        fps=args.fps,
        region=explicit_region,
        auto_region=not args.no_auto_region,
        bottom_ratio=args.bottom_ratio,
        threads=args.threads,
        hwaccel=args.hwaccel,
        skip_frame=args.skip_frame,
        max_analysis_width=args.max_analysis_width,
        segment_seconds=args.segment_seconds,
        timing_workers=args.timing_workers,
        timing_input=args.timing_input,
        export_strips_dir=strips_tmp_dir,
        reuse_cache=reuse_cache,
        quiet=quiet,
    )
    resolved_region = header.resolved_region
    header_ready_offset_s = time.perf_counter() - t_total0
    use_single_pass_strips = (
        strips_tmp_dir is not None
        and header.source in ("sublift_cli_stream", "sublift_cli_segmented")
    )
    if strips_tmp_dir is not None and not use_single_pass_strips:
        shutil.rmtree(strips_tmp_dir, ignore_errors=True)
        active_strips_dir: Path | None = None
    else:
        active_strips_dir = strips_tmp_dir

    # Stage 2 & 3 configuration: Bounded in-memory streaming decode -> Contact Sheet -> VLM API
    crop_box = compute_strip_crop_geometry(header.video_w, header.video_h, resolved_region)
    strip_h = compute_target_strip_height(crop_box, strip_w=960)
    cpu_cnt = os.cpu_count() or 8
    if args.extract_workers > 0:
        extract_workers = args.extract_workers
    elif header.source in ("sublift_cli_stream", "sublift_cli_segmented"):
        # Live Stage 1 already saturates P-cores; cap concurrent Stage 2 workers to avoid thrashing.
        extract_workers = min(3, max(2, cpu_cnt // 4))
    else:
        extract_workers = min(6, max(2, cpu_cnt // 2))

    env_file = args.env_file if args.env_file is not None else (repo_root / ".env")
    vlm_cfg = resolve_vlm_config(
        provider=args.provider,
        endpoint=args.endpoint,
        api_key=args.api_key,
        model=args.model,
        thinking_mode=args.thinking,
        timeout_s=args.timeout_s,
        max_retries=args.max_retries,
        env_file=env_file,
    )

    save_strips_dir = (work_dir / "strips") if save_debug else None
    save_sheets_dir = (work_dir / "sheets") if save_debug else None

    on_slot_failure = str(getattr(args, "on_slot_failure", "paddle") or "paddle")
    fallback_resolver: SlotFallbackResolver | None = None
    if on_slot_failure == "paddle":
        fallback_resolver = PaddleSlotFallbackResolver(
            cli_bin=cli_bin,
            video_path=video_path,
            resolved_region=resolved_region,
            work_dir=work_dir,
            fps=args.fps,
            bottom_ratio=args.bottom_ratio,
            threads=args.threads,
            script=args.script,
            hwaccel=args.hwaccel,
            repo_root=repo_root,
        )
    elif on_slot_failure == "placeholder":
        fallback_resolver = PlaceholderSlotFallbackResolver()
    elif on_slot_failure != "abort":
        raise ValueError(f"Unsupported --on-slot-failure policy: {on_slot_failure!r}")

    with InMemoryStripExtractor(
        video_path,
        crop_box,
        strip_w=960,
        strip_h=strip_h,
        hwaccel=args.hwaccel,
        single_pass_strips_dir=active_strips_dir,
        resolved_region=resolved_region,
        video_w=header.video_w,
        quiet=quiet,
        save_debug_strips_dir=save_strips_dir,
        save_debug_sheets_dir=save_sheets_dir,
    ) as extractor:
        vlm_doc, transcribed_slots, slots, stream_stats = transcribe_streaming_sheets(
            slot_stream,
            extractor.prepare_sheet,
            work_dir,
            config=vlm_cfg,
            slots_per_sheet=args.slots_per_sheet,
            script=args.script,
            strip_w=960,
            strip_h=strip_h,
            extract_workers=extract_workers,
            vlm_workers=args.workers,
            fallback_resolver=fallback_resolver,
            save_debug_sheets=save_debug,
            reuse_cache=reuse_cache,
            quiet=quiet,
        )
        extractor_stats = extractor.stats()

    # Stage 4: Deterministic assembly
    cues, filtered_noise, merged_groups = assemble_cues_from_slots(
        slots,
        transcribed_slots,
        merge_adjacent=not args.no_merge_adjacent,
        max_merge_gap_ms=args.merge_gap_ms,
    )
    write_srt(cues, out_srt)
    (work_dir / "filtered_noise.json").write_text(
        json.dumps(filtered_noise, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    (work_dir / "merged_adjacent.json").write_text(
        json.dumps(merged_groups, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )

    pipeline_wall_s = time.perf_counter() - t_total0
    video_duration_s = header.duration_ms / 1000.0 if header.duration_ms > 0 else 0.0
    realtime_speedup_x = (
        round(video_duration_s / pipeline_wall_s, 3)
        if video_duration_s > 0.0 and pipeline_wall_s > 0.0
        else 0.0
    )
    rtf = (
        round(pipeline_wall_s / video_duration_s, 4)
        if video_duration_s > 0.0
        else 0.0
    )
    first_rel = stream_stats.get("first_sheet_dispatched_at_s")
    first_total_s = (
        round(header_ready_offset_s + float(first_rel), 3)
        if first_rel is not None
        else None
    )
    disk_temp_images_written = (
        stream_stats["in_memory_sheets_encoded"] + len(slots)
        if save_debug
        else 0
    )
    stage1_wall_s = float(timing_meta.get("elapsed_s") or stream_stats["stage1_timing_wall_s"])
    pipeline_telemetry = {
        "mode": "debug_disk_images" if save_debug else "in_memory_streaming",
        "pipeline_wall_s": round(pipeline_wall_s, 3),
        "video_duration_s": round(video_duration_s, 3),
        "realtime_speedup_x": realtime_speedup_x,
        "rtf": rtf,
        "stage1_timing_wall_s": round(stage1_wall_s, 3),
        "stage1_header_wait_s": round(header_ready_offset_s, 3),
        "first_sheet_dispatched_at_s": first_total_s,
        "stage2_decode_sum_s": stream_stats["stage2_decode_sum_s"],
        "stage2_sheet_encode_sum_s": stream_stats["stage2_sheet_encode_sum_s"],
        "stage3_vlm_wall_s": stream_stats["stage3_vlm_wall_s"],
        "stage3_tail_after_timing_s": stream_stats["stage3_tail_after_timing_s"],
        "extract_workers": stream_stats["extract_workers"],
        "vlm_workers": stream_stats["vlm_workers"],
        "max_in_flight_sheets": stream_stats["max_in_flight_sheets"],
        "in_memory_sheets_encoded": stream_stats["in_memory_sheets_encoded"],
        "cached_sheet_hits": stream_stats["cached_sheet_hits"],
        "total_sheet_png_bytes": stream_stats["total_sheet_png_bytes"],
        "disk_temp_images_written": disk_temp_images_written,
        "cv2_seeks": extractor_stats["cv2_seeks"],
        "cv2_grabs": extractor_stats["cv2_grabs"],
        "ffmpeg_fallbacks": extractor_stats["ffmpeg_fallbacks"],
        "single_pass_strip_hits": extractor_stats["single_pass_strip_hits"],
        "hedged_requests": stream_stats.get("hedged_requests", 0),
        "hedged_wins": stream_stats.get("hedged_wins", 0),
    }

    report: dict[str, Any] = {
        "video": str(video_path),
        "out_srt": str(out_srt),
        "work_dir": str(work_dir),
        "model": vlm_cfg.model,
        "endpoint": vlm_cfg.endpoint,
        "thinking_mode": vlm_cfg.thinking_mode,
        "fps": args.fps,
        "resolved_region": resolved_region.to_cli_arg(),
        "strip_crop_box": crop_box.to_cli_arg(),
        "timing_slots": len(slots),
        "contact_sheets": stream_stats["total_sheets"],
        "assembled_cues": len(cues),
        "filtered_noise_slots": len(filtered_noise),
        "merged_adjacent_reduction": max(0, len(slots) - len(filtered_noise) - len(cues)),
        "on_slot_failure": on_slot_failure,
        "degraded_slots": stream_stats.get("degraded_slots", 0),
        "failed_slots": stream_stats.get("failed_slots", []),
        "pipeline_telemetry": pipeline_telemetry,
        "vlm_usage": summarize_vlm_usage(vlm_doc),
        "timing_meta": timing_meta,
    }

    # Optional Stage 5: PaddleOCR baseline & authoritative scripts/srt_eval.py comparisons
    paddle_srt: Path | None = None
    if args.compare_paddle:
        paddle_srt = work_dir / f"{video_path.stem}.paddle.srt"
        if not (reuse_cache and paddle_srt.is_file() and paddle_srt.stat().st_size > 0):
            paddle_region = (
                resolved_region
                if (explicit_region is not None or timing_meta.get("auto_region_detected"))
                else None
            )
            report["paddle_elapsed_s"] = _run_paddle_baseline(
                cli_bin=cli_bin,
                video_path=video_path,
                out_srt=paddle_srt,
                fps=args.fps,
                region=paddle_region,
                bottom_ratio=args.bottom_ratio,
                threads=args.threads,
                script=args.script,
                hwaccel=args.hwaccel,
                skip_frame=args.skip_frame,
                quiet=quiet,
            )
        report["paddle_srt"] = str(paddle_srt)

    evaluations: dict[str, Any] = {}
    if args.gt_srt is not None and args.gt_srt.is_file():
        evaluations["vlm_vs_gt"] = evaluate_srt_pair(
            args.gt_srt, out_srt, iou_thr=args.iou_thr, repo_root=repo_root
        )
        if paddle_srt is not None and paddle_srt.is_file():
            evaluations["paddle_vs_gt"] = evaluate_srt_pair(
                args.gt_srt, paddle_srt, iou_thr=args.iou_thr, repo_root=repo_root
            )

    if paddle_srt is not None and paddle_srt.is_file():
        evaluations["paddle_vs_vlm"] = evaluate_srt_pair(
            out_srt, paddle_srt, iou_thr=args.iou_thr, repo_root=repo_root
        )

    if evaluations:
        report["evaluations"] = evaluations

    report["total_elapsed_s"] = round(time.perf_counter() - t_total0, 3)
    (work_dir / "report.json").write_text(
        json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    if not quiet:
        print(
            f"[Done] Wall={pipeline_wall_s:.2f}s ({realtime_speedup_x:.2f}x RT, RTF={rtf:.4f}) | "
            f"Stage1={stage1_wall_s:.2f}s | FirstSheet=+{first_total_s or 0:.2f}s | "
            f"TailAfterStage1={stream_stats['stage3_tail_after_timing_s']:.2f}s | "
            f"DiskTempImages={disk_temp_images_written} | Cues={len(cues)}",
            file=sys.stderr,
            flush=True,
        )
    return report


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="python3 -m tools.vlm_ocr",
        description="Standalone multimodal VLM OCR tool driven by sublift_cli --timing-only.",
    )
    parser.add_argument("input", type=Path, help="Input video file or directory of videos.")
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=None,
        help="Output .srt file (single video) or output directory (directory mode).",
    )
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=None,
        help="Directory to store output .srt and working artifacts (timing/sheets/report).",
    )
    parser.add_argument(
        "--work-dir",
        type=Path,
        default=None,
        help="Working directory for intermediate timing/strips/sheets/vlm JSON artifacts.",
    )
    parser.add_argument("--cli", type=Path, default=None, help="Explicit path to sublift_cli binary.")
    parser.add_argument(
        "--timing-input",
        type=Path,
        default=None,
        help="Optional pre-computed .jsonl or .srt timing slots file (skips sublift_cli timing).",
    )
    parser.add_argument("--fps", type=float, default=5.0, help="Sampling FPS for timing stage (default: 5.0).")
    parser.add_argument("--region", type=str, default=None, help="Explicit ROI 'x,y,w,h'.")
    parser.add_argument(
        "--no-auto-region",
        action="store_true",
        help="Disable --auto-region and fall back to --bottom-ratio when --region is omitted.",
    )
    parser.add_argument("--bottom-ratio", type=float, default=0.3, help="Bottom crop ratio fallback (default: 0.3).")
    parser.add_argument("--threads", type=int, default=0, help="sublift_cli worker thread count (0 = auto).")
    parser.add_argument(
        "--hwaccel",
        type=str,
        default=_default_hwaccel(),
        help="FFmpeg hardware acceleration mode (default: auto).",
    )
    parser.add_argument(
        "--skip-frame",
        choices=["auto", "none", "noref", "nonref"],
        default=_default_skip_frame(),
        help="FFmpeg reference-only frame skip policy for timing stage (default: auto, enables noref at >=50fps).",
    )
    parser.add_argument(
        "--max-analysis-width",
        type=int,
        default=1920,
        help="Max ROI width forwarded to Stage 1 timing analysis (0 = unscaled, default: 1920).",
    )
    parser.add_argument(
        "--segment-seconds",
        type=float,
        default=0.0,
        help="Optional Stage 1 segment duration seconds for parallel timing (0 = auto).",
    )
    parser.add_argument(
        "--timing-workers",
        type=int,
        default=0,
        help="Stage 1 parallel timing workers (0 = auto single-stream unless high-fps long video).",
    )
    parser.add_argument("--slots-per-sheet", type=int, default=10, help="Strips per Contact Sheet (default: 10).")
    parser.add_argument(
        "--extract-workers",
        "--strip-workers",
        dest="extract_workers",
        type=int,
        default=0,
        help="Concurrent in-memory strip decode & sheet encode workers (0 = auto).",
    )
    parser.add_argument("--workers", type=int, default=10, help="Concurrent VLM API workers (default: 10).")
    parser.add_argument(
        "--save-debug-images",
        "--save-crops",
        dest="save_debug_images",
        action="store_true",
        help="Persist intermediate strips/ and sheets/ PNGs to work-dir (default: pure in-memory streaming).",
    )
    parser.add_argument(
        "--provider",
        choices=["auto", "minimax", "nvidia", "openai"],
        default="auto",
        help="VLM provider profile in .env (default: auto).",
    )
    parser.add_argument("--endpoint", type=str, default=None, help="Override OpenAI-compatible base URL or endpoint.")
    parser.add_argument("--model", type=str, default=None, help="Override VLM model ID (default from .env).")
    parser.add_argument("--api-key", type=str, default=None, help="Override API key (defaults to .env / env).")
    parser.add_argument("--env-file", type=Path, default=None, help="Path to .env file (default: <repo>/.env).")
    parser.add_argument(
        "--thinking",
        choices=["auto", "none", "low", "minimal", "default"],
        default="auto",
        help="Reasoning effort policy (default: auto).",
    )
    parser.add_argument("--timeout-s", type=float, default=120.0, help="Per-sheet HTTP timeout seconds (default: 120).")
    parser.add_argument("--max-retries", type=int, default=4, help="Max retries per sheet (default: 4).")
    parser.add_argument(
        "--on-slot-failure",
        choices=["paddle", "placeholder", "abort"],
        default="paddle",
        help="Fallback strategy when an isolated 1-slot leaf fails VLM OCR (default: paddle).",
    )
    parser.add_argument("--script", choices=["auto", "cjk", "latin"], default="auto", help="Subtitle script mode.")
    parser.add_argument("--merge-gap-ms", type=int, default=200, help="Max gap ms to merge adjacent identical cues.")
    parser.add_argument("--no-merge-adjacent", action="store_true", help="Disable adjacent identical cue merging.")
    parser.add_argument("--gt-srt", type=Path, default=None, help="Optional reference GT .srt for evaluation.")
    parser.add_argument("--iou-thr", type=float, default=0.5, help="Primary temporal IoU threshold (default: 0.5).")
    parser.add_argument(
        "--compare-paddle",
        action="store_true",
        help="Also run local PaddleOCR via sublift_cli on the same ROI and compare via srt_eval.py.",
    )
    parser.add_argument("--recursive", action="store_true", help="Recursively scan directory inputs for videos.")
    parser.add_argument("--no-cache", action="store_true", help="Ignore cached timing, strips, and VLM JSON.")
    parser.add_argument("--quiet", action="store_true", help="Suppress stderr progress logs.")
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    repo_root = Path(__file__).resolve().parents[2]
    videos = _discover_videos(args.input, recursive=args.recursive)

    if len(videos) == 1 and args.input.is_file():
        video = videos[0]
        if args.output is not None:
            out_srt = args.output
            work_dir = args.work_dir or args.out_dir or (out_srt.parent / f"{out_srt.stem}.vlm_work")
        elif args.out_dir is not None:
            work_dir = args.work_dir or args.out_dir
            out_srt = args.out_dir / f"{video.stem}.vlm.srt"
        else:
            work_dir = args.work_dir or (video.parent / f"{video.stem}.vlm_work")
            out_srt = work_dir / f"{video.stem}.vlm.srt"

        report = process_single_video(video, out_srt=out_srt, work_dir=work_dir, args=args, repo_root=repo_root)
        print(json.dumps(report, indent=2, ensure_ascii=False))
        return 0

    base_out = args.out_dir or args.output or (args.input / "vlm_ocr_batch")
    base_out.mkdir(parents=True, exist_ok=True)
    batch_reports: list[dict[str, Any]] = []
    for idx, video in enumerate(videos, 1):
        if not args.quiet:
            print(f"[Batch {idx}/{len(videos)}] Processing {video.name}...", file=sys.stderr, flush=True)
        vid_dir = base_out / video.stem
        out_srt = vid_dir / f"{video.stem}.vlm.srt"
        rep = process_single_video(video, out_srt=out_srt, work_dir=vid_dir, args=args, repo_root=repo_root)
        batch_reports.append(rep)

    summary = {"input_dir": str(args.input), "total_videos": len(videos), "results": batch_reports}
    (base_out / "batch_summary.json").write_text(
        json.dumps(summary, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    print(json.dumps(summary, indent=2, ensure_ascii=False))
    return 0
