"""Adapter delegating timing & region detection strictly to sublift_cli."""

from __future__ import annotations

import json
import os
import queue
import re
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterator, Sequence

from .evaluator import load_authoritative_srt_eval
from .models import CropBox, TimingSlot, TimingStreamHeader


SEGMENT_OVERLAP_MS = 500


@dataclass(frozen=True)
class VideoStreamMeta:
    """Primary video stream metadata probed in a single ffprobe call."""

    width: int
    height: int
    duration_ms: int
    fps: float
    codec_name: str
    pix_fmt: str
    bit_rate_bps: int = 0


_AUTO_REGION_RE = re.compile(
    r"自动区域探测成功:\s*区域=\[x=(\d+),\s*y=(\d+),\s*w=(\d+),\s*h=(\d+)\]"
)
LIVE_CUE_LINE_RE = re.compile(
    r"^\[字幕\s+#(\d+)\]\s+(\d{2}:\d{2}:\d{2}[,.]\d{3})\s+-->\s+(\d{2}:\d{2}:\d{2}[,.]\d{3})"
)


def _srt_ts_to_ms(ts: str) -> int:
    h_str, m_str, rest = ts.strip().replace(".", ",").split(":")
    s_str, ms_str = rest.split(",")
    return ((int(h_str) * 60 + int(m_str)) * 60 + int(s_str)) * 1000 + int(ms_str)


def resolve_sublift_cli(
    explicit_cli: Path | None = None,
    repo_root: Path | None = None,
) -> Path:
    """Locate compiled sublift_cli binary."""
    if explicit_cli is not None:
        if explicit_cli.is_file() and os.access(explicit_cli, os.X_OK):
            return explicit_cli
        raise FileNotFoundError(f"sublift_cli is not executable: {explicit_cli}")

    env_cli = os.environ.get("SUBLIFT_CLI", "").strip()
    if env_cli:
        cand = Path(env_cli)
        if cand.is_file() and os.access(cand, os.X_OK):
            return cand
        raise FileNotFoundError(f"SUBLIFT_CLI points to invalid binary: {cand}")

    root = repo_root or Path(__file__).resolve().parents[2]
    for rel in (
        "build/release/tools/cli/sublift_cli",
        "build/debug/tools/cli/sublift_cli",
    ):
        cand = root / rel
        if cand.is_file() and os.access(cand, os.X_OK):
            return cand

    raise FileNotFoundError(
        "sublift_cli binary not found in build/release or build/debug. "
        "Please build it first (cmake --build build/release --target sublift_cli)."
    )


def _parse_rate(raw: Any) -> float:
    if raw is None:
        return 0.0
    text = str(raw).strip()
    if not text or text == "0/0":
        return 0.0
    if "/" in text:
        num_s, den_s = text.split("/", 1)
        try:
            den = float(den_s)
            return float(num_s) / den if den > 0.0 else 0.0
        except ValueError:
            return 0.0
    try:
        return float(text)
    except ValueError:
        return 0.0


def _parse_positive_int(raw: Any) -> int:
    if raw is None:
        return 0
    try:
        val = round(float(str(raw).strip()))
        return val if val > 0 else 0
    except (ValueError, TypeError):
        return 0


def probe_video_rich_info(video_path: Path) -> VideoStreamMeta:
    """Probe primary video stream metadata (`codec_name`, `pix_fmt`, dimensions, fps, duration, bitrate)."""
    cmd = [
        "ffprobe",
        "-v",
        "error",
        "-select_streams",
        "v:0",
        "-show_entries",
        "stream=codec_name,pix_fmt,width,height,r_frame_rate,avg_frame_rate,duration,bit_rate:format=duration,bit_rate,size",
        "-of",
        "json",
        str(video_path),
    ]
    out = subprocess.check_output(cmd, text=True)
    data = json.loads(out)
    streams = data.get("streams") or []
    if not streams:
        raise RuntimeError(f"No video stream found in {video_path}")
    stream = streams[0]
    fmt = data.get("format") or {}
    width = int(stream["width"])
    height = int(stream["height"])
    duration_ms = 0
    for cand in (fmt.get("duration"), stream.get("duration")):
        if cand is not None:
            val = float(cand)
            if val > 0:
                duration_ms = round(val * 1000)
                break
    fps = _parse_rate(stream.get("avg_frame_rate")) or _parse_rate(stream.get("r_frame_rate"))
    codec_name = str(stream.get("codec_name") or "").strip().lower()
    pix_fmt = str(stream.get("pix_fmt") or "").strip().lower()
    bit_rate_bps = _parse_positive_int(stream.get("bit_rate")) or _parse_positive_int(fmt.get("bit_rate"))
    if bit_rate_bps <= 0 and duration_ms > 0:
        size_bytes = _parse_positive_int(fmt.get("size"))
        if size_bytes > 0:
            bit_rate_bps = round((size_bytes * 8.0) / (duration_ms / 1000.0))
    return VideoStreamMeta(
        width=width,
        height=height,
        duration_ms=duration_ms,
        fps=fps,
        codec_name=codec_name,
        pix_fmt=pix_fmt,
        bit_rate_bps=bit_rate_bps,
    )


def probe_video_stream_info(video_path: Path) -> tuple[int, int, int]:
    """Probe `(width, height, duration_ms)` from `video_path` in a single ffprobe call."""
    meta = probe_video_rich_info(video_path)
    return meta.width, meta.height, meta.duration_ms


def _is_high_bit_pix_fmt(pix_fmt: str) -> bool:
    return any(
        marker in pix_fmt
        for marker in ("10le", "10be", "12le", "12be", "14le", "14be", "16le", "16be", "p010", "p012", "p016")
    )


def _is_videotoolbox_active_for_meta(meta: VideoStreamMeta, hwaccel: str) -> bool:
    if sys.platform != "darwin":
        return False
    norm_hw = (hwaccel or "auto").strip().lower()
    if norm_hw == "videotoolbox":
        return True
    if norm_hw != "auto":
        return False
    heavy_codec = meta.codec_name in {"hevc", "h265", "av1", "libdav1d", "vp9", "prores"}
    high_bit = _is_high_bit_pix_fmt(meta.pix_fmt)
    return (
        heavy_codec
        or high_bit
        or meta.width > 1920
        or meta.height > 1080
        or meta.fps >= 48.0
    )


def _needs_multi_vt_cabac(meta: VideoStreamMeta) -> bool:
    """Return True when CABAC entropy decode is heavy enough to benefit from Multi-VT workers."""
    return meta.bit_rate_bps >= 3_000_000 or (meta.bit_rate_bps <= 0 and meta.fps >= 29.5)


def resolve_timing_parallelism(
    meta: VideoStreamMeta,
    *,
    requested_workers: int = 0,
    segment_seconds: float = 0.0,
    hwaccel: str = "auto",
) -> tuple[int, float]:
    """Resolve `(timing_workers, effective_segment_seconds)` safely across bitrates and codecs."""
    vt_active = _is_videotoolbox_active_for_meta(meta, hwaccel)
    norm_hw = (hwaccel or "auto").strip().lower()
    heavy_codec = meta.codec_name in {"hevc", "h265", "av1", "libdav1d", "vp9", "prores"}
    is_uhd_or_high_bit = meta.width > 1920 or meta.height > 1080 or _is_high_bit_pix_fmt(meta.pix_fmt)

    if requested_workers > 0:
        workers = requested_workers
        seg_sec = segment_seconds if segment_seconds > 0.0 else (900.0 if workers > 1 else 0.0)
    elif (
        meta.duration_ms >= 30 * 60 * 1000
        and (
            meta.fps >= 48.0
            or (heavy_codec and not vt_active and norm_hw in ("none", "off", "false", "0", "auto"))
        )
    ):
        workers = 2
        seg_sec = segment_seconds if segment_seconds > 0.0 else 900.0
    elif (
        not is_uhd_or_high_bit
        and meta.duration_ms >= 35 * 60 * 1000
        and _needs_multi_vt_cabac(meta)
    ):
        # Long high-bitrate <=1080p video: offload CABAC entropy decode across Multi-VT on macOS.
        if sys.platform == "darwin" and norm_hw in ("auto", "videotoolbox"):
            workers = 2 if heavy_codec else 3
        else:
            workers = 2
        seg_sec = segment_seconds if segment_seconds > 0.0 else 600.0
    elif (
        not is_uhd_or_high_bit
        and meta.duration_ms >= 45 * 60 * 1000
        and 0 < meta.bit_rate_bps < 3_000_000
    ):
        # Long low-bitrate <=1080p H.264: 2-way SW segmentation doubles throughput while keeping Stage 2 fast.
        workers = 2
        seg_sec = segment_seconds if segment_seconds > 0.0 else 600.0
    elif segment_seconds > 0.0 and meta.duration_ms > int(segment_seconds * 1000) + 1000:
        workers = 2
        seg_sec = segment_seconds
    else:
        workers = 1
        seg_sec = 0.0

    if is_uhd_or_high_bit and vt_active and workers > 2:
        workers = 2
    elif not is_uhd_or_high_bit and (vt_active or norm_hw == "videotoolbox") and workers > 3:
        workers = 3
    return workers, seg_sec


def resolve_segment_hwaccel(
    meta: VideoStreamMeta,
    hwaccel: str,
    workers: int,
) -> str:
    """Resolve effective per-segment `--hwaccel` flag for multi-worker Stage 1 execution."""
    norm_hw = (hwaccel or "auto").strip().lower()
    heavy_codec = meta.codec_name in {"hevc", "h265", "av1", "libdav1d", "vp9", "prores"}
    is_uhd_or_high_bit = meta.width > 1920 or meta.height > 1080 or _is_high_bit_pix_fmt(meta.pix_fmt)
    if norm_hw == "auto" and sys.platform == "darwin" and workers > 1 and not is_uhd_or_high_bit:
        if _needs_multi_vt_cabac(meta) or heavy_codec or meta.fps >= 48.0:
            return "videotoolbox"
        return "none"
    return hwaccel or "auto"


def compute_segment_windows(
    duration_ms: int,
    segment_seconds: float,
    overlap_ms: int = SEGMENT_OVERLAP_MS,
) -> list[tuple[int, int, int, int]]:
    """Compute `(core_start_ms, core_end_ms, win_start_ms, win_end_ms)` windows with overlap."""
    if duration_ms <= 0:
        return [(0, 0, 0, 0)]
    seg_ms = round(segment_seconds * 1000.0)
    if seg_ms <= 0 or seg_ms >= duration_ms:
        return [(0, duration_ms, 0, duration_ms)]

    windows: list[tuple[int, int, int, int]] = []
    core_start = 0
    while core_start < duration_ms:
        core_end = min(duration_ms, core_start + seg_ms)
        if duration_ms - core_end < 1000:
            core_end = duration_ms
        win_start = max(0, core_start - overlap_ms)
        win_end = min(duration_ms, core_end + overlap_ms)
        windows.append((core_start, core_end, win_start, win_end))
        if core_end >= duration_ms:
            break
        core_start = core_end
    return windows


def _merge_kept_tuples(
    kept: list[tuple[int, int, int]],
    boundaries: Sequence[int],
    boundary_window: int,
    boundary_gap_ms: int,
) -> list[tuple[int, int, int]]:
    if not kept:
        return []
    ordered = sorted(kept, key=lambda item: (item[0], item[1], item[2]))

    def _near_boundary(s_ms: int, e_ms: int) -> bool:
        return any(s_ms <= b + boundary_window and e_ms >= b - boundary_window for b in boundaries)

    merged: list[tuple[int, int, int]] = [ordered[0]]
    for cur_s, cur_e, cur_seg in ordered[1:]:
        prev_s, prev_e, prev_seg = merged[-1]
        if cur_seg != prev_seg and (
            cur_s <= prev_e
            or (_near_boundary(prev_s, cur_e) and (cur_s - prev_e) <= boundary_gap_ms)
        ):
            merged[-1] = (min(prev_s, cur_s), max(prev_e, cur_e), cur_seg)
        else:
            merged.append((cur_s, cur_e, cur_seg))
    return merged


def merge_segment_timing_events(
    segment_intervals: Sequence[tuple[int, int, Sequence[TimingSlot | tuple[int, int]]]],
    *,
    overlap_ms: int = SEGMENT_OVERLAP_MS,
    boundary_gap_ms: int = 220,
) -> list[TimingSlot]:
    """Filter overlap-buffer duplicates and stitch subtitles crossing segment boundaries."""
    ordered = sorted(segment_intervals, key=lambda item: (item[0], item[1]))
    if not ordered:
        return []

    boundaries = [core_end for (_, core_end, _) in ordered[:-1]]
    boundary_window = overlap_ms + 200

    kept: list[tuple[int, int, int]] = []
    for seg_idx, (core_start, core_end, events) in enumerate(ordered):
        for ev in events:
            if isinstance(ev, TimingSlot):
                s_ms, e_ms = ev.start_ms, ev.end_ms
            else:
                s_ms, e_ms = int(ev[0]), int(ev[1])
            if e_ms <= s_ms:
                continue
            # Drop events wholly inside neighbor overlap buffers; each segment owns [core_start, core_end).
            if e_ms <= core_start or s_ms >= core_end:
                continue
            kept.append((s_ms, e_ms, seg_idx))

    merged = _merge_kept_tuples(kept, boundaries, boundary_window, boundary_gap_ms)

    return [
        TimingSlot(
            slot_id=idx,
            index=idx,
            start_ms=s_ms,
            end_ms=e_ms,
            mid_ms=(s_ms + e_ms) // 2,
            duration_ms=e_ms - s_ms,
        )
        for idx, (s_ms, e_ms, _) in enumerate(merged, 1)
    ]


class ProgressiveSegmentMerger:
    """Emit boundary-safe merged TimingSlots live while multi-segment workers execute in parallel.

    For exact batch parity, live events pushed into each open segment must arrive in
    non-decreasing `start_ms` order (standard `sublift_cli --timing-only` order); if any
    segment receives out-of-order `start_ms`, emission safely pauses at that segment until
    `finish_segment(seg_idx)` completes.
    """

    def __init__(
        self,
        windows: Sequence[tuple[int, int, int, int]],
        *,
        overlap_ms: int = SEGMENT_OVERLAP_MS,
        boundary_gap_ms: int = 220,
    ) -> None:
        self._windows = list(windows)
        self._overlap_ms = int(overlap_ms)
        self._boundary_gap_ms = int(boundary_gap_ms)
        self._boundaries = [w[1] for w in self._windows[:-1]]
        self._boundary_window = self._overlap_ms + 200
        n = len(self._windows)
        self._per_seg: list[list[tuple[int, int]]] = [[] for _ in range(n)]
        self._seen: list[set[tuple[int, int]]] = [set() for _ in range(n)]
        self._finished: list[bool] = [False] * n
        self._last_pushed_start: list[int] = [-1] * n
        self._monotonic: list[bool] = [True] * n
        self._cursor = 0
        self._emitted_count = 0

    def push_segment_event(self, seg_idx: int, start_ms: int, end_ms: int) -> list[TimingSlot]:
        if seg_idx < 0 or seg_idx >= len(self._windows) or self._finished[seg_idx]:
            return []
        core_start, core_end, _ws, _we = self._windows[seg_idx]
        s_ms, e_ms = int(start_ms), int(end_ms)
        if e_ms <= s_ms or e_ms <= core_start or s_ms >= core_end:
            return []
        key = (s_ms, e_ms)
        if key in self._seen[seg_idx]:
            return []
        self._seen[seg_idx].add(key)
        if s_ms < self._last_pushed_start[seg_idx]:
            self._monotonic[seg_idx] = False
        self._last_pushed_start[seg_idx] = max(self._last_pushed_start[seg_idx], s_ms)
        self._per_seg[seg_idx].append(key)
        if seg_idx == self._cursor:
            return self._advance()
        return []

    def finish_segment(self, seg_idx: int) -> list[TimingSlot]:
        if seg_idx < 0 or seg_idx >= len(self._windows):
            return []
        if not self._finished[seg_idx]:
            self._finished[seg_idx] = True
            while self._cursor < len(self._windows) and self._finished[self._cursor]:
                self._cursor += 1
        return self._advance()

    def _emit_range(self, merged: list[tuple[int, int, int]], stop_idx: int) -> list[TimingSlot]:
        out: list[TimingSlot] = []
        while self._emitted_count < stop_idx:
            s_ms, e_ms, _seg = merged[self._emitted_count]
            slot_num = self._emitted_count + 1
            self._emitted_count += 1
            out.append(
                TimingSlot(
                    slot_id=slot_num,
                    index=slot_num,
                    start_ms=s_ms,
                    end_ms=e_ms,
                    mid_ms=(s_ms + e_ms) // 2,
                    duration_ms=e_ms - s_ms,
                )
            )
        return out

    def _advance(self) -> list[TimingSlot]:
        n = len(self._windows)
        if self._cursor >= n:
            kept = [
                (s_ms, e_ms, idx)
                for idx in range(n)
                for (s_ms, e_ms) in self._per_seg[idx]
            ]
            merged = _merge_kept_tuples(
                kept, self._boundaries, self._boundary_window, self._boundary_gap_ms
            )
            return self._emit_range(merged, len(merged))

        include_cursor = self._monotonic[self._cursor]
        limit_idx = self._cursor + 1 if include_cursor else self._cursor
        kept = [
            (s_ms, e_ms, idx)
            for idx in range(limit_idx)
            for (s_ms, e_ms) in self._per_seg[idx]
        ]
        if not kept:
            return []
        merged = _merge_kept_tuples(
            kept, self._boundaries, self._boundary_window, self._boundary_gap_ms
        )

        future_candidates: list[int] = []
        for j in range(self._cursor + 1, n):
            future_candidates.append(max(0, self._windows[j][0] - self._overlap_ms))
            for s_j, _ in self._per_seg[j]:
                future_candidates.append(s_j)
        future_min_s = min(future_candidates, default=10**15)

        win_start_cur = max(0, self._windows[self._cursor][0] - self._overlap_ms)
        if include_cursor and self._per_seg[self._cursor]:
            cur_horizon = max(win_start_cur, self._last_pushed_start[self._cursor])
        else:
            cur_horizon = win_start_cur

        min_possible_next_s = min(future_min_s, cur_horizon)
        safe_end_cutoff = min_possible_next_s - self._boundary_window - self._boundary_gap_ms - 50

        stop_idx = self._emitted_count
        while stop_idx < len(merged) and merged[stop_idx][1] < safe_end_cutoff:
            stop_idx += 1
        return self._emit_range(merged, stop_idx)


def parse_auto_region_from_cli_output(stdout_text: str) -> CropBox | None:
    """Parse auto-detected `CropBox` from `sublift_cli` stdout."""
    match = _AUTO_REGION_RE.search(stdout_text)
    if not match:
        return None
    return CropBox(
        x=int(match.group(1)),
        y=int(match.group(2)),
        width=int(match.group(3)),
        height=int(match.group(4)),
    )


def compute_bottom_fallback_box(
    video_w: int,
    video_h: int,
    bottom_ratio: float,
) -> CropBox:
    """Match ExtractionSession::feed fallback bottom crop geometry."""
    if video_w <= 0 or video_h <= 0:
        raise ValueError(f"Invalid video dimensions: {video_w}x{video_h}")
    if 0.0 < bottom_ratio < 1.0:
        h = int(video_h * bottom_ratio)
        if 0 < h <= video_h:
            return CropBox(x=0, y=video_h - h, width=video_w, height=h)
    return CropBox(x=0, y=0, width=video_w, height=video_h)


def load_timing_slots(
    timing_path: Path,
    *,
    repo_root: Path | None = None,
) -> list[TimingSlot]:
    """Load `TimingSlot` list from `.jsonl` or `.srt` produced by sublift_cli --timing-only."""
    if not timing_path.is_file():
        raise FileNotFoundError(f"Timing file not found: {timing_path}")

    raw_intervals: list[tuple[int, int, int]] = []
    if timing_path.suffix.lower() == ".jsonl":
        for idx, line in enumerate(timing_path.read_text(encoding="utf-8").splitlines()):
            line = line.strip()
            if not line:
                continue
            item = json.loads(line)
            start_ms = int(item["start_ms"])
            end_ms = int(item["end_ms"])
            orig_idx = int(item.get("index", idx + 1))
            if end_ms > start_ms:
                raw_intervals.append((orig_idx, start_ms, end_ms))
    elif timing_path.suffix.lower() == ".srt":
        srt_eval = load_authoritative_srt_eval(repo_root)
        for idx, (start_ms, end_ms, _text) in enumerate(srt_eval.parse_srt(str(timing_path)), 1):
            if end_ms > start_ms:
                raw_intervals.append((idx, int(start_ms), int(end_ms)))
    else:
        raise ValueError(f"Unsupported timing file format {timing_path.suffix!r} (expected .jsonl or .srt)")

    slots: list[TimingSlot] = []
    for slot_id, (orig_idx, start_ms, end_ms) in enumerate(raw_intervals, 1):
        slots.append(
            TimingSlot(
                slot_id=slot_id,
                index=orig_idx,
                start_ms=start_ms,
                end_ms=end_ms,
                mid_ms=(start_ms + end_ms) // 2,
                duration_ms=end_ms - start_ms,
            )
        )
    return slots


def _log(msg: str, *, quiet: bool) -> None:
    if not quiet:
        print(msg, file=sys.stderr, flush=True)


def _normalize_skip_frame(raw: str | None) -> str:
    """Normalize `--skip-frame` policy (`auto`, `none`, or `noref`)."""
    cleaned = (raw or "auto").strip().lower()
    if cleaned in ("", "auto"):
        return "auto"
    if cleaned in ("none", "off", "false", "0"):
        return "none"
    if cleaned in ("noref", "nonref"):
        return "noref"
    raise ValueError(f"Unsupported skip_frame mode: {raw!r} (expected auto|none|noref)")


def _build_timing_cli_cmd(
    *,
    cli_bin: Path,
    video_path: Path,
    out_jsonl: Path,
    fps: float,
    bottom_ratio: float,
    threads: int = 0,
    region: CropBox | None = None,
    auto_region: bool = True,
    hwaccel: str = "auto",
    skip_frame: str = "auto",
    max_analysis_width: int = 1920,
    start_ms: int | None = None,
    end_ms: int | None = None,
    export_strips_dir: Path | None = None,
) -> list[str]:
    norm_skip = _normalize_skip_frame(skip_frame)
    cmd = [
        str(cli_bin),
        "--timing-only",
        "-i",
        str(video_path),
        "-o",
        str(out_jsonl),
        "--fps",
        str(fps),
        "--bottom-ratio",
        str(bottom_ratio),
        "--hwaccel",
        hwaccel or "auto",
        "--skip-frame",
        norm_skip,
        "--max-analysis-width",
        str(max(0, int(max_analysis_width))),
    ]
    if threads > 0:
        cmd.extend(["--threads", str(threads)])
    if region is not None:
        cmd.extend(["--region", region.to_cli_arg()])
    elif auto_region:
        cmd.append("--auto-region")
    if start_ms is not None and start_ms > 0:
        cmd.extend(["--start-ms", str(start_ms)])
    if end_ms is not None and end_ms > 0:
        cmd.extend(["--end-ms", str(end_ms)])
    if export_strips_dir is not None:
        cmd.extend(["--export-strips-dir", str(export_strips_dir)])
    return cmd


def _write_timing_jsonl(slots: Sequence[TimingSlot], out_path: Path) -> None:
    lines = [
        json.dumps({"index": s.index, "start_ms": s.start_ms, "end_ms": s.end_ms}, ensure_ascii=False)
        for s in slots
    ]
    out_path.write_text(("\n".join(lines) + "\n") if lines else "", encoding="utf-8")


def run_timing_stage(
    video_path: Path,
    work_dir: Path,
    *,
    cli_bin: Path,
    fps: float = 5.0,
    region: CropBox | None = None,
    auto_region: bool = True,
    bottom_ratio: float = 0.3,
    threads: int = 0,
    hwaccel: str = "auto",
    skip_frame: str = "auto",
    max_analysis_width: int = 1920,
    timing_input: Path | None = None,
    reuse_cache: bool = True,
    quiet: bool = False,
) -> tuple[list[TimingSlot], CropBox, dict[str, Any]]:
    """Run Stage 1 via `sublift_cli --timing-only` (or reuse cached timing output)."""
    norm_skip = _normalize_skip_frame(skip_frame)
    work_dir.mkdir(parents=True, exist_ok=True)
    vid_w, vid_h, duration_ms = probe_video_stream_info(video_path)
    meta_path = work_dir / "timing_meta.json"
    timing_jsonl = work_dir / "timing.jsonl"

    if timing_input is not None:
        slots = load_timing_slots(timing_input)
        resolved = region or compute_bottom_fallback_box(vid_w, vid_h, bottom_ratio)
        meta = {
            "source": "timing_input",
            "timing_path": str(timing_input),
            "video_width": vid_w,
            "video_height": vid_h,
            "duration_ms": duration_ms,
            "fps": fps,
            "skip_frame": norm_skip,
            "slot_count": len(slots),
            "resolved_region": {
                "x": resolved.x,
                "y": resolved.y,
                "width": resolved.width,
                "height": resolved.height,
            },
        }
        meta_path.write_text(json.dumps(meta, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        return slots, resolved, meta

    if reuse_cache and timing_jsonl.is_file() and meta_path.is_file():
        try:
            cached_meta = json.loads(meta_path.read_text(encoding="utf-8"))
            cached_reg = cached_meta.get("resolved_region") or {}
            resolved = region or CropBox(
                x=int(cached_reg["x"]),
                y=int(cached_reg["y"]),
                width=int(cached_reg["width"]),
                height=int(cached_reg["height"]),
            )
            cached_skip = _normalize_skip_frame(str(cached_meta.get("skip_frame", "auto")))
            if (
                abs(float(cached_meta.get("fps", -1.0)) - fps) < 1e-6
                and cached_skip == norm_skip
            ):
                slots = load_timing_slots(timing_jsonl)
                _log(
                    f"[Stage 1] Reused cached timing: {len(slots)} slots, region={resolved.to_cli_arg()}",
                    quiet=quiet,
                )
                return slots, resolved, cached_meta
        except Exception:
            pass

    cmd = _build_timing_cli_cmd(
        cli_bin=cli_bin,
        video_path=video_path,
        out_jsonl=timing_jsonl,
        fps=fps,
        bottom_ratio=bottom_ratio,
        threads=threads,
        region=region,
        auto_region=auto_region,
        hwaccel=hwaccel,
        skip_frame=norm_skip,
        max_analysis_width=max_analysis_width,
    )

    _log(f"[Stage 1] Running sublift_cli --timing-only on {video_path.name} (fps={fps})...", quiet=quiet)
    t0 = time.perf_counter()
    proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
    elapsed_s = time.perf_counter() - t0
    if proc.returncode != 0:
        raise RuntimeError(
            f"sublift_cli --timing-only failed (exit={proc.returncode}):\n"
            f"{proc.stderr.strip() or proc.stdout.strip()}"
        )

    detected_region = parse_auto_region_from_cli_output(proc.stdout)
    resolved = region or detected_region or compute_bottom_fallback_box(vid_w, vid_h, bottom_ratio)
    slots = load_timing_slots(timing_jsonl)

    (work_dir / "sublift_timing.log").write_text(
        proc.stdout + ("\n--- STDERR ---\n" + proc.stderr if proc.stderr else ""),
        encoding="utf-8",
    )

    meta = {
        "source": "sublift_cli",
        "cli_bin": str(cli_bin),
        "timing_path": str(timing_jsonl),
        "video_width": vid_w,
        "video_height": vid_h,
        "duration_ms": duration_ms,
        "fps": fps,
        "hwaccel": hwaccel,
        "skip_frame": norm_skip,
        "max_analysis_width": max_analysis_width,
        "auto_region_requested": region is None and auto_region,
        "auto_region_detected": detected_region is not None,
        "slot_count": len(slots),
        "elapsed_s": round(elapsed_s, 3),
        "resolved_region": {
            "x": resolved.x,
            "y": resolved.y,
            "width": resolved.width,
            "height": resolved.height,
        },
    }
    meta_path.write_text(json.dumps(meta, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    _log(
        f"[Stage 1] Completed in {elapsed_s:.2f}s: {len(slots)} slots, region={resolved.to_cli_arg()}",
        quiet=quiet,
    )
    return slots, resolved, meta


def _run_segmented_timing_stream(
    video_path: Path,
    work_dir: Path,
    *,
    cli_bin: Path,
    vid_meta: VideoStreamMeta,
    fps: float,
    region: CropBox | None,
    auto_region: bool,
    bottom_ratio: float,
    threads: int,
    hwaccel: str,
    skip_frame: str,
    max_analysis_width: int,
    workers: int,
    segment_seconds: float,
    export_strips_dir: Path | None = None,
    quiet: bool,
) -> tuple[TimingStreamHeader, Iterator[TimingSlot], dict[str, Any]]:
    """Run progressive multi-segment parallel Stage 1 with live boundary-safe slot streaming."""
    norm_skip = _normalize_skip_frame(skip_frame)
    windows = compute_segment_windows(vid_meta.duration_ms, segment_seconds, overlap_ms=SEGMENT_OVERLAP_MS)
    seg_dir = work_dir / "segments"
    seg_dir.mkdir(parents=True, exist_ok=True)
    if export_strips_dir is not None:
        export_strips_dir.mkdir(parents=True, exist_ok=True)
    timing_jsonl = work_dir / "timing.jsonl"
    meta_path = work_dir / "timing_meta.json"

    t0 = time.perf_counter()
    detected_region: CropBox | None = None
    if region is not None:
        resolved = region
    elif auto_region:
        probe_end_ms = 250 if vid_meta.duration_ms <= 0 else max(250, min(500, vid_meta.duration_ms))
        probe_jsonl = seg_dir / "region_probe.jsonl"
        probe_cmd = _build_timing_cli_cmd(
            cli_bin=cli_bin,
            video_path=video_path,
            out_jsonl=probe_jsonl,
            fps=min(fps, 2.0),
            bottom_ratio=bottom_ratio,
            threads=threads,
            region=None,
            auto_region=True,
            hwaccel=hwaccel,
            skip_frame=norm_skip,
            max_analysis_width=max_analysis_width,
            end_ms=probe_end_ms,
        )
        probe_proc = subprocess.run(probe_cmd, capture_output=True, text=True, check=False)
        if probe_proc.returncode == 0:
            detected_region = parse_auto_region_from_cli_output(probe_proc.stdout)
        resolved = detected_region or compute_bottom_fallback_box(
            vid_meta.width, vid_meta.height, bottom_ratio
        )
    else:
        resolved = compute_bottom_fallback_box(vid_meta.width, vid_meta.height, bottom_ratio)

    seg_hwaccel = resolve_segment_hwaccel(vid_meta, hwaccel, workers)
    per_worker_threads = (
        threads
        if threads > 0
        else max(2, min(4, (os.cpu_count() or 8) // max(2, workers * 2)))
    )
    _log(
        f"[Stage 1] Progressive segmented timing ({len(windows)} segments, workers={workers}, "
        f"hwaccel={seg_hwaccel}, threads/seg={per_worker_threads}, region={resolved.to_cli_arg()})...",
        quiet=quiet,
    )

    meta: dict[str, Any] = {
        "source": "sublift_cli_segmented",
        "cli_bin": str(cli_bin),
        "timing_path": str(timing_jsonl),
        "video_width": vid_meta.width,
        "video_height": vid_meta.height,
        "duration_ms": vid_meta.duration_ms,
        "fps": fps,
        "codec_name": vid_meta.codec_name,
        "pix_fmt": vid_meta.pix_fmt,
        "bit_rate_bps": vid_meta.bit_rate_bps,
        "hwaccel": hwaccel,
        "segment_hwaccel": seg_hwaccel,
        "skip_frame": norm_skip,
        "max_analysis_width": max_analysis_width,
        "segment_seconds": segment_seconds,
        "timing_workers": workers,
        "segment_count": len(windows),
        "auto_region_requested": region is None and auto_region,
        "auto_region_detected": detected_region is not None,
        "slot_count": 0,
        "elapsed_s": 0.0,
        "resolved_region": {
            "x": resolved.x,
            "y": resolved.y,
            "width": resolved.width,
            "height": resolved.height,
        },
    }
    header = TimingStreamHeader(
        video_w=vid_meta.width,
        video_h=vid_meta.height,
        duration_ms=vid_meta.duration_ms,
        fps=fps,
        resolved_region=resolved,
        auto_region_detected=detected_region is not None,
        source="sublift_cli_segmented",
    )

    def _progressive_slot_iter() -> Iterator[TimingSlot]:
        event_q: queue.Queue[tuple[str, int, Any]] = queue.Queue()
        stop_event = threading.Event()
        procs_lock = threading.Lock()
        active_procs: set[subprocess.Popen[str]] = set()
        seg_logs: dict[int, str] = {}
        merger = ProgressiveSegmentMerger(windows, overlap_ms=SEGMENT_OVERLAP_MS)
        emitted_slots: list[TimingSlot] = []

        def _terminate_proc(proc: subprocess.Popen[str]) -> None:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=3.0)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
            if proc.stdout is not None:
                try:
                    proc.stdout.close()
                except Exception:
                    pass

        def _run_seg_worker(seg_idx: int, win: tuple[int, int, int, int]) -> None:
            if stop_event.is_set():
                return
            _core_start, _core_end, win_start, win_end = win
            seg_jsonl = seg_dir / f"seg_{seg_idx:02d}.jsonl"
            cmd = _build_timing_cli_cmd(
                cli_bin=cli_bin,
                video_path=video_path,
                out_jsonl=seg_jsonl,
                fps=fps,
                bottom_ratio=bottom_ratio,
                threads=per_worker_threads,
                region=resolved,
                auto_region=False,
                hwaccel=seg_hwaccel,
                skip_frame=norm_skip,
                max_analysis_width=max_analysis_width,
                start_ms=win_start,
                end_ms=win_end,
                export_strips_dir=export_strips_dir,
            )
            proc: subprocess.Popen[str] | None = None
            seg_log_lines: list[str] = []
            try:
                proc = subprocess.Popen(
                    cmd,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                    encoding="utf-8",
                    errors="replace",
                    bufsize=1,
                )
                with procs_lock:
                    if stop_event.is_set():
                        _terminate_proc(proc)
                        return
                    active_procs.add(proc)

                if proc.stdout is not None:
                    while not stop_event.is_set():
                        line = proc.stdout.readline()
                        if not line:
                            break
                        seg_log_lines.append(line)
                        match = LIVE_CUE_LINE_RE.match(line.strip())
                        if match:
                            s_ms = _srt_ts_to_ms(match.group(2))
                            e_ms = _srt_ts_to_ms(match.group(3))
                            if e_ms > s_ms:
                                event_q.put(("cue", seg_idx, (s_ms, e_ms)))

                rc = proc.wait()
                if stop_event.is_set():
                    return
                log_text = "".join(seg_log_lines)
                if rc != 0:
                    tail = "".join(seg_log_lines[-30:]).strip()
                    raise RuntimeError(
                        f"sublift_cli segment #{seg_idx} [{win_start}..{win_end}ms] failed (exit={rc}):\n{tail}"
                    )
                if seg_jsonl.is_file():
                    for extra in load_timing_slots(seg_jsonl):
                        event_q.put(("cue", seg_idx, (extra.start_ms, extra.end_ms)))
                event_q.put(("done", seg_idx, log_text))
            except Exception as exc:
                if not stop_event.is_set():
                    event_q.put(("err", seg_idx, exc))
            finally:
                if proc is not None:
                    with procs_lock:
                        active_procs.discard(proc)
                    if proc.stdout is not None:
                        try:
                            proc.stdout.close()
                        except Exception:
                            pass

        pool = ThreadPoolExecutor(max_workers=max(1, workers))
        try:
            for idx, win in enumerate(windows):
                pool.submit(_run_seg_worker, idx, win)

            done_segments = 0
            while done_segments < len(windows):
                kind, seg_idx, payload = event_q.get()
                if kind == "err":
                    raise payload
                if kind == "cue":
                    s_ms, e_ms = payload
                    for slot in merger.push_segment_event(seg_idx, s_ms, e_ms):
                        emitted_slots.append(slot)
                        yield slot
                elif kind == "done":
                    seg_logs[seg_idx] = payload
                    done_segments += 1
                    for slot in merger.finish_segment(seg_idx):
                        emitted_slots.append(slot)
                        yield slot

            elapsed_s = time.perf_counter() - t0
            _write_timing_jsonl(emitted_slots, timing_jsonl)
            (work_dir / "sublift_timing.log").write_text(
                "\n".join(
                    f"=== SEGMENT {idx} ===\n{seg_logs.get(idx, '')}"
                    for idx in range(len(windows))
                ),
                encoding="utf-8",
            )
            meta["slot_count"] = len(emitted_slots)
            meta["elapsed_s"] = round(elapsed_s, 3)
            meta_path.write_text(
                json.dumps(meta, indent=2, ensure_ascii=False) + "\n",
                encoding="utf-8",
            )
            _log(
                f"[Stage 1] Segmented streaming completed in {elapsed_s:.2f}s: "
                f"{len(emitted_slots)} slots, region={resolved.to_cli_arg()}",
                quiet=quiet,
            )
        finally:
            stop_event.set()
            with procs_lock:
                remaining = list(active_procs)
            for p in remaining:
                _terminate_proc(p)
            pool.shutdown(wait=True, cancel_futures=True)

    return header, _progressive_slot_iter(), meta


def stream_timing_stage(
    video_path: Path,
    work_dir: Path,
    *,
    cli_bin: Path,
    fps: float = 5.0,
    region: CropBox | None = None,
    auto_region: bool = True,
    bottom_ratio: float = 0.3,
    threads: int = 0,
    hwaccel: str = "auto",
    skip_frame: str = "auto",
    max_analysis_width: int = 1920,
    segment_seconds: float = 0.0,
    timing_workers: int = 0,
    timing_input: Path | None = None,
    export_strips_dir: Path | None = None,
    reuse_cache: bool = True,
    quiet: bool = False,
) -> tuple[TimingStreamHeader, Iterator[TimingSlot], dict[str, Any]]:
    """Resolve Stage-1 header eagerly and stream closed TimingSlots live as sublift_cli emits them."""
    norm_skip = _normalize_skip_frame(skip_frame)
    work_dir.mkdir(parents=True, exist_ok=True)
    vid_w, vid_h, duration_ms = probe_video_stream_info(video_path)
    meta_path = work_dir / "timing_meta.json"
    timing_jsonl = work_dir / "timing.jsonl"

    if timing_input is not None:
        slots = load_timing_slots(timing_input)
        resolved = region or compute_bottom_fallback_box(vid_w, vid_h, bottom_ratio)
        meta: dict[str, Any] = {
            "source": "timing_input",
            "timing_path": str(timing_input),
            "video_width": vid_w,
            "video_height": vid_h,
            "duration_ms": duration_ms,
            "fps": fps,
            "skip_frame": norm_skip,
            "slot_count": len(slots),
            "elapsed_s": 0.0,
            "resolved_region": {
                "x": resolved.x,
                "y": resolved.y,
                "width": resolved.width,
                "height": resolved.height,
            },
        }
        meta_path.write_text(json.dumps(meta, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        header = TimingStreamHeader(
            video_w=vid_w,
            video_h=vid_h,
            duration_ms=duration_ms,
            fps=fps,
            resolved_region=resolved,
            auto_region_detected=False,
            source="external_file",
        )
        return header, iter(slots), meta

    if reuse_cache and timing_jsonl.is_file() and meta_path.is_file():
        try:
            cached_meta = json.loads(meta_path.read_text(encoding="utf-8"))
            cached_reg = cached_meta.get("resolved_region") or {}
            resolved = region or CropBox(
                x=int(cached_reg["x"]),
                y=int(cached_reg["y"]),
                width=int(cached_reg["width"]),
                height=int(cached_reg["height"]),
            )
            cached_skip = _normalize_skip_frame(str(cached_meta.get("skip_frame", "auto")))
            if (
                abs(float(cached_meta.get("fps", -1.0)) - fps) < 1e-6
                and cached_skip == norm_skip
            ):
                slots = load_timing_slots(timing_jsonl)
                _log(
                    f"[Stage 1] Reused cached timing: {len(slots)} slots, region={resolved.to_cli_arg()}",
                    quiet=quiet,
                )
                header = TimingStreamHeader(
                    video_w=vid_w,
                    video_h=vid_h,
                    duration_ms=duration_ms,
                    fps=fps,
                    resolved_region=resolved,
                    auto_region_detected=bool(cached_meta.get("auto_region_detected", False)),
                    source="sublift_cli_cached",
                )
                return header, iter(slots), cached_meta
        except Exception:
            pass

    vid_meta = VideoStreamMeta(
        width=vid_w,
        height=vid_h,
        duration_ms=duration_ms,
        fps=0.0,
        codec_name="",
        pix_fmt="",
    )
    if video_path.is_file() and video_path.stat().st_size > 0:
        try:
            vid_meta = probe_video_rich_info(video_path)
        except Exception:
            pass

    eff_workers, eff_segment_seconds = resolve_timing_parallelism(
        vid_meta,
        requested_workers=timing_workers,
        segment_seconds=segment_seconds,
        hwaccel=hwaccel,
    )
    if eff_workers > 1 and eff_segment_seconds > 0.0:
        windows = compute_segment_windows(vid_meta.duration_ms, eff_segment_seconds)
        if len(windows) > 1:
            return _run_segmented_timing_stream(
                video_path,
                work_dir,
                cli_bin=cli_bin,
                vid_meta=vid_meta,
                fps=fps,
                region=region,
                auto_region=auto_region,
                bottom_ratio=bottom_ratio,
                threads=threads,
                hwaccel=hwaccel,
                skip_frame=norm_skip,
                max_analysis_width=max_analysis_width,
                workers=eff_workers,
                segment_seconds=eff_segment_seconds,
                export_strips_dir=export_strips_dir,
                quiet=quiet,
            )

    if export_strips_dir is not None:
        export_strips_dir.mkdir(parents=True, exist_ok=True)

    cmd = _build_timing_cli_cmd(
        cli_bin=cli_bin,
        video_path=video_path,
        out_jsonl=timing_jsonl,
        fps=fps,
        bottom_ratio=bottom_ratio,
        threads=threads,
        region=region,
        auto_region=auto_region,
        hwaccel=hwaccel,
        skip_frame=norm_skip,
        max_analysis_width=max_analysis_width,
        export_strips_dir=export_strips_dir,
    )

    _log(f"[Stage 1] Streaming sublift_cli --timing-only on {video_path.name} (fps={fps})...", quiet=quiet)
    t0 = time.perf_counter()
    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        bufsize=1,
    )
    assert proc.stdout is not None

    log_lines: list[str] = []
    pending_lines: list[str] = []
    detected_region: CropBox | None = None

    try:
        if region is not None:
            resolved = region
        elif auto_region:
            while True:
                line = proc.stdout.readline()
                if not line:
                    rc = proc.wait()
                    (work_dir / "sublift_timing.log").write_text("".join(log_lines), encoding="utf-8")
                    if rc != 0:
                        tail = "".join(log_lines[-30:]).strip()
                        raise RuntimeError(f"sublift_cli --timing-only failed (exit={rc}):\n{tail}")
                    break
                log_lines.append(line)
                detected_region = parse_auto_region_from_cli_output(line)
                if detected_region is not None:
                    break
                stripped = line.strip()
                if LIVE_CUE_LINE_RE.match(stripped):
                    pending_lines.append(line)
                    break
                if "开始处理视频:" in line or "配置参数:" in line:
                    break
            resolved = detected_region or compute_bottom_fallback_box(vid_w, vid_h, bottom_ratio)
        else:
            resolved = compute_bottom_fallback_box(vid_w, vid_h, bottom_ratio)
    except Exception:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=3.0)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        proc.stdout.close()
        raise

    meta = {
        "source": "sublift_cli",
        "cli_bin": str(cli_bin),
        "timing_path": str(timing_jsonl),
        "video_width": vid_w,
        "video_height": vid_h,
        "duration_ms": duration_ms,
        "fps": fps,
        "codec_name": vid_meta.codec_name,
        "pix_fmt": vid_meta.pix_fmt,
        "hwaccel": hwaccel,
        "skip_frame": norm_skip,
        "max_analysis_width": max_analysis_width,
        "timing_workers": eff_workers,
        "auto_region_requested": region is None and auto_region,
        "auto_region_detected": detected_region is not None,
        "slot_count": 0,
        "elapsed_s": 0.0,
        "resolved_region": {
            "x": resolved.x,
            "y": resolved.y,
            "width": resolved.width,
            "height": resolved.height,
        },
    }
    header = TimingStreamHeader(
        video_w=vid_w,
        video_h=vid_h,
        duration_ms=duration_ms,
        fps=fps,
        resolved_region=resolved,
        auto_region_detected=detected_region is not None,
        source="sublift_cli_stream",
    )

    def _slot_iter() -> Iterator[TimingSlot]:
        slot_id = 0
        try:
            def _parse_cue(raw_line: str) -> TimingSlot | None:
                nonlocal slot_id
                match = LIVE_CUE_LINE_RE.match(raw_line.strip())
                if not match:
                    return None
                start_ms = _srt_ts_to_ms(match.group(2))
                end_ms = _srt_ts_to_ms(match.group(3))
                if end_ms <= start_ms:
                    return None
                slot_id += 1
                return TimingSlot(
                    slot_id=slot_id,
                    index=int(match.group(1)),
                    start_ms=start_ms,
                    end_ms=end_ms,
                    mid_ms=(start_ms + end_ms) // 2,
                    duration_ms=end_ms - start_ms,
                )

            for pl in pending_lines:
                slot = _parse_cue(pl)
                if slot is not None:
                    yield slot

            while True:
                line = proc.stdout.readline()  # type: ignore[union-attr]
                if not line:
                    break
                log_lines.append(line)
                slot = _parse_cue(line)
                if slot is not None:
                    yield slot

            rc = proc.wait()
            elapsed_s = time.perf_counter() - t0
            (work_dir / "sublift_timing.log").write_text("".join(log_lines), encoding="utf-8")
            if rc != 0:
                tail = "".join(log_lines[-30:]).strip()
                raise RuntimeError(f"sublift_cli --timing-only failed (exit={rc}):\n{tail}")

            # Safety net in case stdout cue lines missed any trailing finalized interval.
            if timing_jsonl.is_file():
                final_slots = load_timing_slots(timing_jsonl)
                if len(final_slots) > slot_id:
                    for extra in final_slots[slot_id:]:
                        slot_id += 1
                        yield extra

            meta["slot_count"] = slot_id
            meta["elapsed_s"] = round(elapsed_s, 3)
            meta_path.write_text(
                json.dumps(meta, indent=2, ensure_ascii=False) + "\n",
                encoding="utf-8",
            )
            _log(
                f"[Stage 1] Stream completed in {elapsed_s:.2f}s: {slot_id} slots, region={resolved.to_cli_arg()}",
                quiet=quiet,
            )
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=3.0)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
            if proc.stdout is not None:
                proc.stdout.close()

    return header, _slot_iter(), meta
