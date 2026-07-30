"""用变化点状态机检测 1:00~2:00 的字幕时间轴，与真实 SRT 逐段比对。"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

import cv2
import numpy as np

from sublift.config import DEFAULT_CONFIG
from sublift.detector import BottomCropDetector
from sublift.extractor import FfmpegExtractor
from sublift.pipeline.changepoint import ChangePointDetector, EventType
from sublift.pipeline.signature import compute_signature
from sublift.pipeline.timeline import TimelineBuilder


@dataclass
class SrtEntry:
    start_ms: int
    end_ms: int
    text: str


_SRT_TIME_RE = re.compile(
    r"(\d{2}):(\d{2}):(\d{2}),(\d{3})\s*-->\s*(\d{2}):(\d{2}):(\d{2}),(\d{3})"
)


def _srt_time_to_ms(h: str, m: str, s: str, ms: str) -> int:
    return int(h) * 3600000 + int(m) * 60000 + int(s) * 1000 + int(ms)


def parse_srt(path: Path) -> list[SrtEntry]:
    entries: list[SrtEntry] = []
    text_lines: list[str] = []
    start_ms = end_ms = 0
    for line in path.read_text(encoding="utf-8").splitlines():
        m = _SRT_TIME_RE.match(line)
        if m:
            if text_lines:
                entries.append(SrtEntry(start_ms, end_ms, "".join(text_lines)))
                text_lines = []
            start_ms = _srt_time_to_ms(m[1], m[2], m[3], m[4])
            end_ms = _srt_time_to_ms(m[5], m[6], m[7], m[8])
        elif line.strip() and not line.strip().isdigit():
            text_lines.append(line.strip())
    if text_lines:
        entries.append(SrtEntry(start_ms, end_ms, "".join(text_lines)))
    return entries


def srt_at_ms(entries: list[SrtEntry], ts_ms: int) -> str:
    for e in entries:
        if e.start_ms <= ts_ms < e.end_ms:
            return e.text
    return ""


def event_label(event_type: EventType) -> str:
    return {EventType.IN: "IN", EventType.OUT: "OUT", EventType.CHANGE: "CHG"}.get(
        event_type, "???"
    )


def main() -> None:
    video = Path("debug/Zootopia_clip_hardsub1.mkv")
    srt_path = Path("debug/Zootopia_cn.srt")
    out_path = Path("debug/timeline_compare_result.txt")
    start_ms = 60_000
    end_ms = 120_000

    srt_all = parse_srt(srt_path)
    srt_window = [e for e in srt_all if e.start_ms < end_ms and e.end_ms > start_ms]

    extractor = FfmpegExtractor(fps=DEFAULT_CONFIG.sample_fps)
    detector = BottomCropDetector(bottom_ratio=0.3)
    cp = ChangePointDetector()
    builder = TimelineBuilder()

    raw_signals: list[tuple[int, float, int]] = []

    for frame in extractor.extract(video):
        ts = frame.timestamp_ms
        if ts < start_ms:
            continue
        if ts >= end_ms:
            break

        region = detector.detect(frame)
        b = region.box
        crop_pil = frame.image.crop((b.x, b.y, b.x + b.width, b.y + b.height))
        crop_np = cv2.cvtColor(np.asarray(crop_pil), cv2.COLOR_RGB2BGR)

        sig = compute_signature(crop_np, ts)
        raw_signals.append((sig.timestamp_ms, sig.foreground_ratio, sig.dhash))

        event = cp.process(sig, crop_np)
        if event:
            builder.consume(event)

    builder.finalize_open_segment(end_ms - 1000)
    segments = builder.build()

    lines: list[str] = []
    lines.append("=" * 80)
    lines.append("原始信号（前景占比 + dHash）")
    lines.append("=" * 80)
    lines.append(f" {'时间戳':>8s} | {'前景占比':>8s} | {'dHash':<20s} | {'实际字幕':<30s}")
    lines.append("-" * 8 + "-+-" + "-" * 8 + "-+-" + "-" * 20 + "-+-" + "-" * 30)
    for ts, fg, dh in raw_signals:
        actual = srt_at_ms(srt_window, ts).replace("\n", " ") or "无"
        lines.append(f" {ts:>7d}ms | {fg:>8.4f} | {dh:<20d} | {actual:<30s}")

    lines.append("")
    lines.append("=" * 80)
    lines.append("检测到的字幕段 vs 实际 SRT 条目")
    lines.append("=" * 80)
    lines.append(
        f" {'段号':>4s} | {'检测起止':<28s} | {'时长':>6s} | {'实际起止':<28s} | {'匹配':>4s}"
    )
    lines.append("-" * 4 + "-+-" + "-" * 28 + "-+-" + "-" * 6 + "-+-" + "-" * 28 + "-+-" + "-" * 4)

    def ms_to_srt(ms: int) -> str:
        h, rem = divmod(ms, 3600000)
        m, rem = divmod(rem, 60000)
        s, ms_ = divmod(rem, 1000)
        return f"{h:02d}:{m:02d}:{s:02d},{ms_:03d}"

    def find_best_match(seg_start: int, seg_end: int) -> tuple[str, str, bool]:
        best = None
        best_overlap = 0
        for e in srt_window:
            overlap = max(0, min(seg_end, e.end_ms) - max(seg_start, e.start_ms))
            if overlap > best_overlap:
                best_overlap = overlap
                best = e
        if best is None or best_overlap == 0:
            return ("无", "", False)
        return (
            f"{ms_to_srt(best.start_ms)} → {ms_to_srt(best.end_ms)}",
            best.text[:28],
            best_overlap >= (seg_end - seg_start) * 0.5,
        )

    for i, seg in enumerate(segments):
        seg_end = seg.end_ms if seg.end_ms is not None else end_ms
        seg_range = f"{ms_to_srt(seg.start_ms)} → {ms_to_srt(seg_end)}"
        actual_range, actual_text, matched = find_best_match(seg.start_ms, seg_end)
        duration = seg_end - seg.start_ms
        match_str = "✓" if matched else "✗"
        lines.append(
            f" {i:>3d}  | {seg_range:<28s} | {duration:>5d}ms | "
            f"{actual_range:<28s} | {match_str:>4s}"
        )
        if actual_text:
            lines.append(f"     | {'':28s} | {'':6s} | {actual_text:<28s} |")

    total_detected = sum((s.end_ms or end_ms) - s.start_ms for s in segments)
    total_actual = sum(min(e.end_ms, end_ms) - max(e.start_ms, start_ms) for e in srt_window)
    lines.append("")
    lines.append(f"检测到 {len(segments)} 个字幕段，总时长 {total_detected}ms")
    lines.append(f"实际有 {len(srt_window)} 个字幕段，总时长 {total_actual}ms")

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"结果已写入 {out_path}")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
