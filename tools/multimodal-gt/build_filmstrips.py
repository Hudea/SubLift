#!/usr/bin/env python3
"""
Standalone Multimodal GT Filmstrip Builder for SubLift.

Supports two modes:
  - `strict-rle` (Scheme B, default): 10fps continuous strip extraction + 16-Grid foreground stroke mask
    max-block XOR check + min hold length + 2-frame boundary safety margins. Reduces page count by ~76%
    with zero loss on timing boundaries or short subtitles.
  - `uniform` (Scheme A): 10fps lossless uniform strip layout without static hold folding.
  - `both`: Generates both Scheme A and Scheme B pages side by side for A/B comparison.

Only depends on `ffmpeg`, `ffprobe`, `numpy`, and `Pillow` (decoupled from product runtime).
"""
from __future__ import annotations

import argparse
import json
import subprocess
import time
from pathlib import Path
from typing import Any

import numpy as np
from PIL import Image, ImageDraw, ImageFont


def load_font(size: int) -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
    for path in [
        "/System/Library/Fonts/Menlo.ttc",
        "/System/Library/Fonts/SFNSMono.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
    ]:
        if Path(path).exists():
            try:
                return ImageFont.truetype(path, size)
            except Exception:
                pass
    return ImageFont.load_default()


def probe_video_dimensions(video_path: Path) -> tuple[int, int]:
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
    out = subprocess.check_output(cmd, text=True)
    data = json.loads(out)
    stream = data["streams"][0]
    return int(stream["width"]), int(stream["height"])


def format_ts(frame_idx: int, fps: int) -> str:
    ms_total = int(round(frame_idx * (1000.0 / fps)))
    m, rem = divmod(ms_total, 60000)
    s, ms = divmod(rem, 1000)
    return f"{m:02d}:{s:02d}.{ms:03d}"


def extract_all_strips(
    video_path: Path,
    raw_strip_dir: Path,
    *,
    fps: int,
    crop_x: int,
    crop_y: int,
    crop_w: int,
    crop_h: int,
    strip_w: int,
    strip_h: int,
    reuse_cache: bool = True,
) -> list[Path]:
    raw_strip_dir.mkdir(parents=True, exist_ok=True)
    existing = sorted(raw_strip_dir.glob("frame_*.jpg"))
    if reuse_cache and len(existing) > 0:
        print(f"[extract] Reusing {len(existing)} cached {fps}fps strips in {raw_strip_dir}")
        return existing

    for p in existing:
        p.unlink()

    vf = f"fps={fps},crop={crop_w}:{crop_h}:{crop_x}:{crop_y},scale={strip_w}:{strip_h}:flags=lanczos"
    cmd = [
        "ffmpeg",
        "-y",
        "-v",
        "error",
        "-i",
        str(video_path),
        "-vf",
        vf,
        "-q:v",
        "2",
        "-start_number",
        "0",
        str(raw_strip_dir / "frame_%04d.jpg"),
    ]
    t0 = time.time()
    subprocess.run(cmd, check=True)
    files = sorted(raw_strip_dir.glob("frame_*.jpg"))
    print(f"[extract] Extracted {len(files)} strips at {fps}fps in {time.time() - t0:.2f}s")
    return files


def compute_stroke_mask(rgb: np.ndarray) -> np.ndarray:
    """
    Extract bright/high-contrast foreground mask (works for white/yellow/colored bright subtitles
    and outline edges) while ignoring dark/smooth background motion.
    """
    r = rgb[:, :, 0].astype(np.int16)
    g = rgb[:, :, 1].astype(np.int16)
    b = rgb[:, :, 2].astype(np.int16)
    luma = (77 * r + 150 * g + 29 * b) >> 8
    max_c = np.maximum(np.maximum(r, g), b)

    gx = np.zeros_like(luma)
    gy = np.zeros_like(luma)
    gx[:, 1:-1] = np.abs(luma[:, 2:] - luma[:, :-2])
    gy[1:-1, :] = np.abs(luma[2:, :] - luma[:-2, :])
    grad = np.maximum(gx, gy)

    bright = (luma >= 180) | ((max_c >= 210) & (grad >= 25))
    has_edge_neighbor = (
        np.pad(grad >= 35, 1, mode="constant", constant_values=False)[1:-1, 1:-1]
        | (grad >= 35)
    )
    return bright & (has_edge_neighbor | (luma >= 215))


def is_same_visual_state(
    mask_a: np.ndarray,
    mask_b: np.ndarray,
    *,
    num_blocks: int = 16,
    max_block_xor_threshold: float = 0.22,
) -> tuple[bool, float]:
    """
    16-Grid Max-Block difference check on the foreground stroke mask.
    """
    _, w = mask_a.shape
    block_w = w // num_blocks
    max_score = 0.0
    for k in range(num_blocks):
        x0 = k * block_w
        x1 = w if k == num_blocks - 1 else (k + 1) * block_w
        ba = mask_a[:, x0:x1]
        bb = mask_b[:, x0:x1]
        union = int(np.count_nonzero(ba | bb))
        if union < 10:
            continue
        xor = int(np.count_nonzero(ba ^ bb))
        ratio = xor / max(union, 25)
        if ratio > max_score:
            max_score = ratio
    return (max_score <= max_block_xor_threshold), max_score


def build_uniform_items(files: list[Path], active_counts: list[int]) -> list[dict[str, Any]]:
    """
    Scheme A: Uniform 10fps lossless items.
    Only collapses a leading studio-logo blank prefix (if >= 30 consecutive blank frames at start)
    into a single folded blank entry, then emits every single 10fps frame without folding.
    """
    n = len(files)
    first_active = 0
    while first_active < n and active_counts[first_active] < 25:
        first_active += 1

    items: list[dict[str, Any]] = []
    lead_end = max(0, first_active - 10)
    if lead_end >= 30:
        items.append({
            "start_frame": 0,
            "end_frame": lead_end - 1,
            "img_path": str(files[lead_end // 2]),
            "kind": "folded_blank",
        })
        start_idx = lead_end
    else:
        start_idx = 0

    for idx in range(start_idx, n):
        items.append({
            "start_frame": idx,
            "end_frame": idx,
            "img_path": str(files[idx]),
            "kind": "single",
        })
    return items


def build_strict_rle_items(
    files: list[Path],
    masks: list[np.ndarray],
    active_counts: list[int],
    *,
    num_blocks: int = 16,
    max_block_xor: float = 0.22,
    min_hold_frames: int = 6,
    margin_frames: int = 2,
) -> tuple[list[dict[str, Any]], list[tuple[int, int]]]:
    """
    Scheme B: 10fps + Strict 16-Grid RLE Folding with boundary safety margins.
    """
    n = len(files)
    runs: list[tuple[int, int]] = []
    run_start = 0
    for idx in range(1, n):
        same, _ = is_same_visual_state(
            masks[run_start], masks[idx], num_blocks=num_blocks, max_block_xor_threshold=max_block_xor
        )
        same_prev, _ = is_same_visual_state(
            masks[idx - 1], masks[idx], num_blocks=num_blocks, max_block_xor_threshold=max_block_xor
        )
        if not (same and same_prev):
            runs.append((run_start, idx - 1))
            run_start = idx
    runs.append((run_start, n - 1))

    items: list[dict[str, Any]] = []
    min_fold_len = max(min_hold_frames, 2 * margin_frames + 2)
    for rs, re in runs:
        length = re - rs + 1
        mean_active = sum(active_counts[rs : re + 1]) / length
        is_blank = mean_active < 25

        if is_blank and length >= 4:
            items.append({"start_frame": rs, "end_frame": rs, "img_path": str(files[rs]), "kind": "single"})
            if re - 1 >= rs + 1:
                mid = (rs + re) // 2
                items.append({
                    "start_frame": rs + 1,
                    "end_frame": re - 1,
                    "img_path": str(files[mid]),
                    "kind": "folded_blank",
                })
            items.append({"start_frame": re, "end_frame": re, "img_path": str(files[re]), "kind": "single"})
        elif not is_blank and length >= min_fold_len:
            for offset in range(margin_frames):
                f_idx = rs + offset
                items.append({"start_frame": f_idx, "end_frame": f_idx, "img_path": str(files[f_idx]), "kind": "single"})
            mid = (rs + re) // 2
            items.append({
                "start_frame": rs + margin_frames,
                "end_frame": re - margin_frames,
                "img_path": str(files[mid]),
                "kind": "folded_hold",
            })
            for offset in range(margin_frames - 1, -1, -1):
                f_idx = re - offset
                items.append({"start_frame": f_idx, "end_frame": f_idx, "img_path": str(files[f_idx]), "kind": "single"})
        else:
            for idx in range(rs, re + 1):
                items.append({
                    "start_frame": idx,
                    "end_frame": idx,
                    "img_path": str(files[idx]),
                    "kind": "single",
                })
    return items, runs


def render_grid_pages(
    items: list[dict[str, Any]],
    out_dir: Path,
    *,
    prefix: str,
    title_prefix: str,
    fps: int,
    strip_w: int,
    strip_h: int,
    rows_per_col: int = 16,
    cols: int = 2,
    overlap_items: int = 2,
) -> list[Path]:
    out_dir.mkdir(parents=True, exist_ok=True)
    for old in out_dir.glob("*.png"):
        old.unlink()

    per_page = rows_per_col * cols
    step = max(1, per_page - overlap_items)
    label_w = 215
    col_gap = 12
    row_gap = 3
    header_h = 42

    col_w = label_w + strip_w
    canvas_w = cols * col_w + (cols - 1) * col_gap
    canvas_h = header_h + rows_per_col * (strip_h + row_gap)

    font_title = load_font(16)
    font_mono = load_font(14)
    font_small = load_font(12)

    generated: list[Path] = []
    page_idx = 0
    start = 0
    while start < len(items):
        batch = items[start : start + per_page]
        if not batch:
            break
        page_idx += 1
        canvas = Image.new("RGB", (canvas_w, canvas_h), (18, 20, 24))
        draw = ImageDraw.Draw(canvas)

        f_first = batch[0]["start_frame"]
        f_last = batch[-1]["end_frame"]
        draw.rectangle([0, 0, canvas_w, header_h], fill=(28, 32, 40))
        draw.text(
            (12, 11),
            f"{title_prefix} | Page {page_idx:02d} | Frames F{f_first:04d}..F{f_last:04d} ({format_ts(f_first, fps)} - {format_ts(f_last, fps)})",
            fill=(235, 240, 248),
            font=font_title,
        )

        for i, item in enumerate(batch):
            c = i // rows_per_col
            r = i % rows_per_col
            x0 = c * (col_w + col_gap)
            y0 = header_h + r * (strip_h + row_gap)

            strip_img = Image.open(item["img_path"]).convert("RGB")
            canvas.paste(strip_img, (x0 + label_w, y0))

            kind = item.get("kind", "single")
            if kind == "folded_blank":
                bg = (215, 222, 232)
            elif kind == "folded_hold":
                bg = (254, 243, 199)
            else:
                bg = (245, 247, 250) if r % 2 == 0 else (232, 236, 242)

            draw.rectangle([x0, y0, x0 + label_w - 3, y0 + strip_h], fill=bg)
            sf = item["start_frame"]
            ef = item["end_frame"]
            if sf == ef:
                draw.text((x0 + 8, y0 + 12), f"F{sf:04d}", fill=(15, 23, 42), font=font_mono)
                draw.text((x0 + 8, y0 + 40), f"{format_ts(sf, fps)}", fill=(185, 28, 28), font=font_mono)
            else:
                count = ef - sf + 1
                tag = f"BLANK x{count}" if kind == "folded_blank" else f"HOLD x{count}"
                draw.text((x0 + 8, y0 + 8), f"F{sf:04d}~F{ef:04d}", fill=(15, 23, 42), font=font_mono)
                draw.text(
                    (x0 + 8, y0 + 29),
                    f"[{tag}]",
                    fill=(30, 64, 175) if kind == "folded_blank" else (146, 64, 14),
                    font=font_small,
                )
                draw.text(
                    (x0 + 8, y0 + 47),
                    f"{format_ts(sf, fps)}~{format_ts(ef, fps)}",
                    fill=(185, 28, 28),
                    font=font_small,
                )

        out_path = out_dir / f"{prefix}_page_{page_idx:02d}.png"
        canvas.save(out_path, optimize=True)
        generated.append(out_path)

        if start + per_page >= len(items):
            break
        start += step

    return generated


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build Multimodal GT Filmstrip pages from a hard-subtitled video.")
    parser.add_argument(
        "--video",
        type=Path,
        default=Path("/Volumes/lab/pp/SubLift/debug/Zootopia_clip_1080p.mp4"),
        help="Input video path.",
    )
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=Path("/Volumes/lab/pp/SubLift/debug/gt_experiment"),
        help="Output working directory for strips, pages, and stats.",
    )
    parser.add_argument(
        "--mode",
        choices=["strict-rle", "uniform", "both"],
        default="strict-rle",
        help="Filmstrip generation mode: strict-rle (Scheme B), uniform (Scheme A), or both.",
    )
    parser.add_argument("--fps", type=int, default=10, help="Uniform extraction frame rate (default: 10).")
    parser.add_argument("--crop-x-ratio", type=float, default=160.0 / 1920.0, help="Normalized crop X start.")
    parser.add_argument("--crop-y-ratio", type=float, default=770.0 / 1080.0, help="Normalized crop Y start.")
    parser.add_argument("--crop-w-ratio", type=float, default=1600.0 / 1920.0, help="Normalized crop width.")
    parser.add_argument("--crop-h-ratio", type=float, default=192.0 / 1080.0, help="Normalized crop height.")
    parser.add_argument("--strip-w", type=int, default=640, help="Scaled strip width in pixels.")
    parser.add_argument("--strip-h", type=int, default=76, help="Scaled strip height in pixels.")
    parser.add_argument("--rows", type=int, default=16, help="Rows per column on each page.")
    parser.add_argument("--cols", type=int, default=2, help="Columns per page.")
    parser.add_argument("--overlap", type=int, default=2, help="Overlapping strips between consecutive pages.")
    parser.add_argument("--max-block-xor", type=float, default=0.22, help="Strict 16-grid max block XOR ratio.")
    parser.add_argument("--min-hold-frames", type=int, default=6, help="Minimum static hold frames before folding.")
    parser.add_argument("--margin-frames", type=int, default=2, help="Unfolded safety margin frames at each boundary.")
    parser.add_argument("--no-cache", action="store_true", help="Force re-extraction of raw strips.")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    video_w, video_h = probe_video_dimensions(args.video)
    crop_x = max(0, int(round(video_w * args.crop_x_ratio)))
    crop_y = max(0, int(round(video_h * args.crop_y_ratio)))
    crop_w = min(video_w - crop_x, int(round(video_w * args.crop_w_ratio)))
    crop_h = min(video_h - crop_y, int(round(video_h * args.crop_h_ratio)))

    raw_strip_dir = args.out_dir / f"raw_{args.fps}fps_strips"
    files = extract_all_strips(
        args.video,
        raw_strip_dir,
        fps=args.fps,
        crop_x=crop_x,
        crop_y=crop_y,
        crop_w=crop_w,
        crop_h=crop_h,
        strip_w=args.strip_w,
        strip_h=args.strip_h,
        reuse_cache=not args.no_cache,
    )

    t0 = time.time()
    masks: list[np.ndarray] = []
    active_counts: list[int] = []
    for f in files:
        arr = np.asarray(Image.open(f).convert("RGB"))
        m = compute_stroke_mask(arr)
        masks.append(m)
        active_counts.append(int(np.count_nonzero(m)))
    print(f"[mask] Computed {len(files)} stroke masks in {time.time() - t0:.2f}s")

    stats: dict[str, Any] = {
        "video": str(args.video),
        "fps": args.fps,
        "total_frames": len(files),
    }

    if args.mode in ("uniform", "both"):
        items_a = build_uniform_items(files, active_counts)
        pages_a = render_grid_pages(
            items_a,
            args.out_dir / "scheme_a_uniform",
            prefix="schemeA",
            title_prefix=f"Scheme A (Uniform {args.fps}fps)",
            fps=args.fps,
            strip_w=args.strip_w,
            strip_h=args.strip_h,
            rows_per_col=args.rows,
            cols=args.cols,
            overlap_items=args.overlap,
        )
        stats["scheme_a_strips"] = len(items_a)
        stats["scheme_a_pages"] = len(pages_a)

    if args.mode in ("strict-rle", "both"):
        items_b, runs = build_strict_rle_items(
            files,
            masks,
            active_counts,
            max_block_xor=args.max_block_xor,
            min_hold_frames=args.min_hold_frames,
            margin_frames=args.margin_frames,
        )
        pages_b = render_grid_pages(
            items_b,
            args.out_dir / "scheme_b_folded",
            prefix="schemeB",
            title_prefix=f"Scheme B (Strict 16-Grid RLE Folded {args.fps}fps)",
            fps=args.fps,
            strip_w=args.strip_w,
            strip_h=args.strip_h,
            rows_per_col=args.rows,
            cols=args.cols,
            overlap_items=args.overlap,
        )
        stats["scheme_b_runs"] = len(runs)
        stats["scheme_b_strips"] = len(items_b)
        stats["scheme_b_pages"] = len(pages_b)

    if "scheme_a_pages" in stats and "scheme_b_pages" in stats:
        stats["strip_reduction_ratio"] = round(1.0 - stats["scheme_b_strips"] / stats["scheme_a_strips"], 4)
        stats["page_reduction_ratio"] = round(1.0 - stats["scheme_b_pages"] / stats["scheme_a_pages"], 4)

    args.out_dir.mkdir(parents=True, exist_ok=True)
    (args.out_dir / "stats.json").write_text(json.dumps(stats, indent=2, ensure_ascii=False))
    print(json.dumps(stats, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
