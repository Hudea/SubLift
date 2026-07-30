"""用 Vision OCR 识别视频 1:00~2:00 的字幕，与真实 SRT 逐秒比对。"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

from sublift.detector import BottomCropDetector
from sublift.extractor import FfmpegExtractor
from sublift.ocr.vision import VisionOcrEngine


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


def text_at_ms(entries: list[SrtEntry], ts_ms: int) -> str:
    for e in entries:
        if e.start_ms <= ts_ms < e.end_ms:
            return e.text
    return ""


def main() -> None:
    video = Path("debug/Zootopia_clip_hardsub1.mkv")
    srt_path = Path("debug/Zootopia_cn.srt")
    out_path = Path("debug/ocr_compare_result.txt")
    start_ms = 60_000
    end_ms = 120_000

    srt_entries = parse_srt(srt_path)
    srt_entries = [e for e in srt_entries if e.start_ms < end_ms and e.end_ms > start_ms]

    extractor = FfmpegExtractor(fps=1.0)
    detector = BottomCropDetector(bottom_ratio=0.3)
    ocr = VisionOcrEngine()

    lines: list[str] = []
    header = f" {'时间戳':>8s} | {'OCR 识别结果':<38s} | {'实际字幕':<38s}"
    sep = "-" * 8 + "-+-" + "-" * 38 + "-+-" + "-" * 38
    lines.append(header)
    lines.append(sep)

    for frame in extractor.extract(video):
        ts = frame.timestamp_ms
        if ts < start_ms:
            continue
        if ts >= end_ms:
            break

        region = detector.detect(frame)
        b = region.box
        cropped = frame.image.crop((b.x, b.y, b.x + b.width, b.y + b.height))
        result = ocr.recognize(cropped)

        ocr_text = result.text.strip().replace("\n", " ") if result.text else "无"
        actual = text_at_ms(srt_entries, ts).replace("\n", " ") or "无"

        lines.append(f" {ts:>7d}ms | {ocr_text:<38s} | {actual:<38s}")

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"结果已写入 {out_path}")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
