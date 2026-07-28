"""端到端管线测试：用真实视频跑通完整 pipeline，与 SRT 逐条比对。"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

from sublift.config import ChangePointConfig, Config, SignatureConfig
from sublift.detector import BottomCropDetector
from sublift.extractor import FfmpegExtractor
from sublift.models import SubtitleEntry
from sublift.ocr.vision import VisionOcrEngine
from sublift.pipeline import Pipeline


@dataclass
class SrtEntry:
    index: int
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
    idx = 0
    for line in path.read_text(encoding="utf-8").splitlines():
        m = _SRT_TIME_RE.match(line)
        if m:
            if text_lines:
                idx += 1
                entries.append(SrtEntry(idx, start_ms, end_ms, "".join(text_lines)))
                text_lines = []
            start_ms = _srt_time_to_ms(m[1], m[2], m[3], m[4])
            end_ms = _srt_time_to_ms(m[5], m[6], m[7], m[8])
        elif line.strip() and not line.strip().isdigit():
            text_lines.append(line.strip())
    if text_lines:
        idx += 1
        entries.append(SrtEntry(idx, start_ms, end_ms, "".join(text_lines)))
    return entries


def ms_to_srt(ms: int) -> str:
    h, rem = divmod(ms, 3600000)
    m, rem = divmod(rem, 60000)
    s, ms_ = divmod(rem, 1000)
    return f"{h:02d}:{m:02d}:{s:02d},{ms_:03d}"


def overlap_ms(a_start: int, a_end: int, b_start: int, b_end: int) -> int:
    return max(0, min(a_end, b_end) - max(a_start, b_start))


def best_match(
    entry: SubtitleEntry, srt_entries: list[SrtEntry]
) -> tuple[SrtEntry | None, int, bool]:
    best = None
    best_overlap = 0
    for s in srt_entries:
        o = overlap_ms(entry.start_ms, entry.end_ms, s.start_ms, s.end_ms)
        if o > best_overlap:
            best_overlap = o
            best = s
    if best is None or best_overlap == 0:
        return (None, 0, False)
    entry_dur = entry.end_ms - entry.start_ms
    matched = best_overlap >= entry_dur * 0.5 or best_overlap >= (best.end_ms - best.start_ms) * 0.5
    return (best, best_overlap, matched)


def main() -> None:
    video = Path("debug/Zootopia_clip_hardsub1.mkv")
    srt_path = Path("debug/Zootopia_cn.srt")
    out_path = Path("debug/pipeline_e2e_result.txt")
    start_ms = 60_000
    end_ms = 120_000

    srt_all = parse_srt(srt_path)
    srt_window = [e for e in srt_all if e.start_ms < end_ms and e.end_ms > start_ms]

    config = Config(
        sample_fps=5.0,
        region_bottom_ratio=0.3,
        confidence_threshold=0.5,
        merge_gap_ms=1000,
        min_duration_ms=500,
        signature=SignatureConfig(
            block_size_ratio=0.08,
            adaptive_c=12,
            hash_size=8,
        ),
        change_point=ChangePointConfig(
            presence_threshold=0.01,
            hysteresis_frames=2,
            change_threshold=10,
            enable_ssim_verify=False,
        ),
    )

    extractor = FfmpegExtractor(fps=config.sample_fps)
    detector = BottomCropDetector(bottom_ratio=config.region_bottom_ratio)
    ocr = VisionOcrEngine()
    pipeline = Pipeline(extractor=extractor, detector=detector, ocr=ocr, config=config)

    entries = pipeline.run(video)

    entries_window = [e for e in entries if e.start_ms < end_ms and e.end_ms > start_ms]

    lines: list[str] = []
    lines.append("=" * 100)
    lines.append(f"Pipeline 端到端测试：{video.name}  {ms_to_srt(start_ms)} → {ms_to_srt(end_ms)}")
    lines.append(
        f"采样率 {config.sample_fps}fps  检测到 {len(entries_window)} 段  实际 {len(srt_window)} 段"
    )
    lines.append("=" * 100)

    header = (
        f" {'#':>3s} | {'检测起止':<26s} | {'时长':>6s} | {'OCR 文本':<38s} | "
        f"{'匹配 SRT':<26s} | {'匹配':>4s}"
    )
    sep = (
        "-" * 3
        + "-+-"
        + "-" * 26
        + "-+-"
        + "-" * 6
        + "-+-"
        + "-" * 38
        + "-+-"
        + "-" * 26
        + "-+-"
        + "-" * 4
    )
    lines.append(header)
    lines.append(sep)

    hit_count = 0
    miss_count = 0
    false_positive_count = 0
    total_time_error = 0
    time_error_count = 0

    matched_srt_indices: set[int] = set()

    for i, entry in enumerate(entries_window):
        match, _ov, matched = best_match(entry, srt_window)

        det_range = f"{ms_to_srt(entry.start_ms)} → {ms_to_srt(entry.end_ms)}"
        duration = entry.end_ms - entry.start_ms
        ocr_text = entry.text.strip().replace("\n", " ") if entry.text else "(空)"

        if match is None:
            false_positive_count += 1
            match_str = "FP"
            srt_range = ""
            srt_text = ""
        elif matched:
            hit_count += 1
            matched_srt_indices.add(match.index)
            match_str = "✓"
            srt_range = f"{ms_to_srt(match.start_ms)} → {ms_to_srt(match.end_ms)}"
            srt_text = match.text[:36]
            time_err = abs(entry.start_ms - match.start_ms)
            total_time_error += time_err
            time_error_count += 1
        else:
            miss_count += 1
            match_str = "✗"
            srt_range = f"{ms_to_srt(match.start_ms)} → {ms_to_srt(match.end_ms)}"
            srt_text = match.text[:36]

        lines.append(
            f" {i:>2d}  | {det_range:<26s} | {duration:>5d}ms | {ocr_text:<38s} | "
            f"{srt_range:<26s} | {match_str:>4s}"
        )
        if srt_text:
            lines.append(f"     | {'':26s} | {'':6s} | {'':38s} | {srt_text:<26s} |")

    lines.append("")
    lines.append("=" * 100)
    lines.append("统计")
    lines.append("=" * 100)
    lines.append(f"  检测段数:          {len(entries_window)}")
    lines.append(f"  实际段数:          {len(srt_window)}")
    lines.append(f"  命中 (✓):          {hit_count}")
    lines.append(f"  漏检 (✗):          {miss_count}")
    lines.append(f"  误检 (FP):         {false_positive_count}")
    lines.append(f"  未匹配 SRT 条目:   {len(srt_window) - len(matched_srt_indices)}")
    if time_error_count > 0:
        lines.append(f"  平均时间偏差:      {total_time_error // time_error_count}ms")

    recall = hit_count / len(srt_window) * 100 if srt_window else 0
    precision = hit_count / len(entries_window) * 100 if entries_window else 0
    lines.append(f"  召回率 (Recall):   {recall:.1f}%")
    lines.append(f"  精确率 (Precision): {precision:.1f}%")

    lines.append("")
    lines.append("-" * 100)
    lines.append("未匹配的 SRT 条目:")
    for s in srt_window:
        if s.index not in matched_srt_indices:
            lines.append(
                f"  #{s.index:>3d}  {ms_to_srt(s.start_ms)} → {ms_to_srt(s.end_ms)}  {s.text[:60]}"
            )

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"结果已写入 {out_path}")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
