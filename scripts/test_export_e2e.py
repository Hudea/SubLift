"""端到端导出测试：pipeline → SRT 导出 → 与真实 SRT 逐条比对。"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

from sublift.config import ChangePointConfig, Config, SignatureConfig
from sublift.detector import BottomCropDetector
from sublift.export import SrtExporter
from sublift.extractor import FfmpegExtractor
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


def text_match_score(detected: str, expected: str) -> float:
    """计算文本相似度（0.0~1.0），基于公共字符比例。"""
    if not expected:
        return 1.0 if not detected else 0.0
    common = sum(1 for c in detected if c in expected)
    return common / max(len(expected), len(detected))


def main() -> None:
    video = Path("debug/Zootopia_clip_hardsub1.mkv")
    srt_path = Path("debug/Zootopia_cn.srt")
    out_srt = Path("debug/pipeline_exported.srt")
    out_report = Path("debug/export_e2e_report.txt")
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

    SrtExporter().export(entries_window, out_srt)
    exported = parse_srt(out_srt)

    lines: list[str] = []
    lines.append("=" * 110)
    lines.append(f"导出 SRT 文件: {out_srt}")
    lines.append(f"检测 {len(exported)} 条  vs  真实 {len(srt_window)} 条")
    lines.append("=" * 110)

    header = (
        f" {'#':>3s} | {'检测起止':<26s} | {'检测文本':<38s} | "
        f"{'匹配 SRT':<26s} | {'文本':>5s} | {'时间':>4s}"
    )
    sep = (
        "-" * 3 + "-+-" + "-" * 26 + "-+-" + "-" * 38 + "-+-"
        + "-" * 26 + "-+-" + "-" * 5 + "-+-" + "-" * 4
    )
    lines.append(header)
    lines.append(sep)

    hit_count = 0
    total_text_score = 0.0
    total_time_error = 0
    time_error_count = 0
    matched_srt_indices: set[int] = set()

    for i, entry in enumerate(exported):
        best = None
        best_overlap = 0
        for s in srt_window:
            o = overlap_ms(entry.start_ms, entry.end_ms, s.start_ms, s.end_ms)
            if o > best_overlap:
                best_overlap = o
                best = s

        det_range = f"{ms_to_srt(entry.start_ms)} → {ms_to_srt(entry.end_ms)}"
        det_text = entry.text.strip().replace("\n", " ") if entry.text else "(空)"

        if best is None or best_overlap == 0:
            lines.append(
                f" {i:>2d}  | {det_range:<26s} | {det_text:<38s} | "
                f"{'':26s} | {'':5s} | {'FP':>4s}"
            )
            continue

        entry_dur = entry.end_ms - entry.start_ms
        best_dur = best.end_ms - best.start_ms
        time_matched = (
            best_overlap >= entry_dur * 0.5 or best_overlap >= best_dur * 0.5
        )
        tscore = text_match_score(entry.text, best.text)
        time_err = abs(entry.start_ms - best.start_ms)

        srt_range = f"{ms_to_srt(best.start_ms)} → {ms_to_srt(best.end_ms)}"
        srt_text = best.text[:36]

        if time_matched:
            hit_count += 1
            matched_srt_indices.add(best.index)
            total_text_score += tscore
            total_time_error += time_err
            time_error_count += 1
            match_str = "✓"
        else:
            match_str = "✗"

        lines.append(
            f" {i:>2d}  | {det_range:<26s} | {det_text:<38s} | "
            f"{srt_range:<26s} | {tscore:.2f} | {match_str:>4s}"
        )
        if srt_text:
            lines.append(
                f"     | {'':26s} | {'':38s} | {srt_text:<26s} | {'':5s} |"
            )

    lines.append("")
    lines.append("=" * 110)
    lines.append("统计")
    lines.append("=" * 110)
    lines.append(f"  导出条目:         {len(exported)}")
    lines.append(f"  实际条目:         {len(srt_window)}")
    lines.append(f"  时间匹配:         {hit_count}")
    lines.append(f"  未匹配 SRT:       {len(srt_window) - len(matched_srt_indices)}")
    if time_error_count > 0:
        lines.append(f"  平均时间偏差:     {total_time_error // time_error_count}ms")
        lines.append(f"  平均文本相似度:   {total_text_score / time_error_count:.2f}")

    recall = hit_count / len(srt_window) * 100 if srt_window else 0
    precision = hit_count / len(exported) * 100 if exported else 0
    lines.append(f"  召回率 (Recall):  {recall:.1f}%")
    lines.append(f"  精确率 (Precision): {precision:.1f}%")

    lines.append("")
    lines.append("-" * 110)
    lines.append("未匹配的 SRT 条目:")
    for s in srt_window:
        if s.index not in matched_srt_indices:
            lines.append(
                f"  #{s.index:>3d}  {ms_to_srt(s.start_ms)} → "
                f"{ms_to_srt(s.end_ms)}  {s.text[:60]}"
            )

    lines.append("")
    lines.append("-" * 110)
    lines.append(f"导出 SRT 内容 ({out_srt}):")
    lines.append(out_srt.read_text(encoding="utf-8"))

    out_report.parent.mkdir(parents=True, exist_ok=True)
    out_report.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"报告已写入 {out_report}")
    print(f"SRT 已导出到 {out_srt}")


if __name__ == "__main__":
    main()
