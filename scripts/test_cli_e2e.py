"""CLI 端到端验收 + benchmark。

两个核心指标：
1. 打轴精度——时间轴召回率/精确率（时间重叠 ≥ 50% 判定匹配）
2. OCR 质量——字符准确率（1-CER，Levenshtein 编辑距离）

输出：
- debug/cli_benchmark_report.txt：汇总报告
- debug/cli_comparison.txt：逐条对比（检测条目 vs 最佳匹配真实条目）

使用 1080p 测试视频（由 4K 原始 clip 缩放生成）。
"""

from __future__ import annotations

import re
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path


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


def levenshtein(s1: str, s2: str) -> int:
    """编辑距离（替换/删除/插入各算 1）。"""
    if len(s1) < len(s2):
        s1, s2 = s2, s1
    if len(s2) == 0:
        return len(s1)
    prev = list(range(len(s2) + 1))
    for i, c1 in enumerate(s1):
        curr = [i + 1]
        for j, c2 in enumerate(s2):
            curr.append(
                min(
                    prev[j + 1] + 1,
                    curr[j] + 1,
                    prev[j] + (c1 != c2),
                )
            )
        prev = curr
    return prev[-1]


def cer(detected: str, reference: str) -> float:
    """字符错误率 = edit_distance / len(reference)。"""
    if not reference:
        return 0.0 if not detected else 1.0
    return levenshtein(detected, reference) / len(reference)


def main() -> None:
    video = Path("debug/Zootopia_clip_1080p.mkv")
    srt_path = Path("debug/Zootopia_cn.srt")
    out_srt = Path("debug/cli_exported.srt")
    out_report = Path("debug/cli_benchmark_report.txt")
    out_comparison = Path("debug/cli_comparison.txt")

    if not video.exists():
        print(f"错误：测试视频不存在: {video}", file=sys.stderr)
        sys.exit(1)

    srt_all = parse_srt(srt_path)
    video_duration_ms = 254_000
    srt_window = [e for e in srt_all if e.start_ms < video_duration_ms]

    cmd = [
        "uv",
        "run",
        "sublift",
        "extract",
        str(video),
        "-o",
        str(out_srt),
        "--fps",
        "5",
        "--engine",
        "vision",
    ]

    print(f"运行 CLI: {' '.join(cmd)}")
    start = time.perf_counter()
    result = subprocess.run(cmd, capture_output=True, text=True)
    elapsed = time.perf_counter() - start

    print(result.stdout)
    if result.returncode != 0:
        print(f"CLI 失败（退出码 {result.returncode}）", file=sys.stderr)
        print(result.stderr, file=sys.stderr)
        sys.exit(1)

    exported = parse_srt(out_srt)

    # --- 逐条匹配 ---
    matches: list[dict[str, object]] = []
    matched_srt_indices: set[int] = set()

    for entry in exported:
        best = None
        best_overlap = 0
        for s in srt_window:
            o = overlap_ms(entry.start_ms, entry.end_ms, s.start_ms, s.end_ms)
            if o > best_overlap:
                best_overlap = o
                best = s

        if best is None or best_overlap == 0:
            matches.append(
                {
                    "detected": entry,
                    "real": None,
                    "overlap": 0,
                    "time_match": False,
                    "edit_dist": None,
                    "cer": None,
                }
            )
            continue

        entry_dur = entry.end_ms - entry.start_ms
        best_dur = best.end_ms - best.start_ms
        time_match = best_overlap >= entry_dur * 0.5 or best_overlap >= best_dur * 0.5

        if time_match:
            matched_srt_indices.add(best.index)

        edit_dist = levenshtein(entry.text, best.text) if entry.text and best.text else None
        cer_val = edit_dist / len(best.text) if edit_dist is not None and best.text else None

        matches.append(
            {
                "detected": entry,
                "real": best,
                "overlap": best_overlap,
                "time_match": time_match,
                "edit_dist": edit_dist,
                "cer": cer_val,
            }
        )

    # --- 指标 1：打轴精度 ---
    time_matched_count = sum(1 for m in matches if m["time_match"])
    recall = time_matched_count / len(srt_window) * 100 if srt_window else 0
    precision = time_matched_count / len(exported) * 100 if exported else 0

    # --- 指标 2：OCR 质量 ---
    cer_values = [m["cer"] for m in matches if m["cer"] is not None and m["time_match"]]
    empty_count = sum(1 for e in exported if not e.text.strip())
    avg_cer = sum(cer_values) / len(cer_values) if cer_values else 0
    char_accuracy = (1 - avg_cer) * 100 if cer_values else 0

    # --- 汇总报告 ---
    lines: list[str] = []
    lines.append("=" * 80)
    lines.append("CLI Benchmark 报告")
    lines.append("=" * 80)
    lines.append("")

    lines.append("测试环境")
    lines.append(f"  视频:       {video.name}（1920x1080, 254s）")
    lines.append("  采样率:     5fps")
    lines.append("  OCR 引擎:   vision")
    lines.append(f"  处理耗时:   {elapsed:.1f}s（{254 / elapsed:.1f}x 实时）")
    lines.append("")

    lines.append("指标 1：打轴精度")
    recall_line = f"  召回率 (Recall):    {recall:.1f}%  ({time_matched_count}/{len(srt_window)})"
    prec_line = f"  精确率 (Precision): {precision:.1f}%  ({time_matched_count}/{len(exported)})"
    lines.append(recall_line)
    lines.append(prec_line)
    lines.append("  判定标准: 时间重叠 ≥ 任一方时长的 50%")
    lines.append("")

    lines.append("指标 2：OCR 质量")
    lines.append(f"  字符准确率 (1-CER):  {char_accuracy:.1f}%")
    lines.append(f"  平均 CER:            {avg_cer * 100:.1f}%")
    lines.append(f"  空文本条目:          {empty_count}/{len(exported)}")
    lines.append("  判定标准: Levenshtein 编辑距离 / 参考文本长度")
    lines.append("")

    out_report.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"报告: {out_report}")

    # --- 逐条对比文件 ---
    comp_lines: list[str] = []
    comp_lines.append("=" * 80)
    comp_lines.append("逐条对比（检测 vs 真实）")
    comp_lines.append("=" * 80)
    comp_lines.append("")
    comp_lines.append(
        f"{'#':>3} | {'检测时间':<26} | {'检测文本':<40} | "
        f"{'真实时间':<26} | {'真实文本':<40} | {'时间':>4} | {'CER':>5}"
    )
    comp_lines.append("-" * 160)

    for i, m in enumerate(matches):
        d = m["detected"]
        r = m["real"]
        det_time = f"{ms_to_srt(d.start_ms)}→{ms_to_srt(d.end_ms)}"
        det_text = d.text[:38] if d.text else "(空)"
        if r is not None:
            real_time = f"{ms_to_srt(r.start_ms)}→{ms_to_srt(r.end_ms)}"
            real_text = r.text[:38] if r.text else "(空)"
        else:
            real_time = ""
            real_text = ""
        tm = "✓" if m["time_match"] else "✗"
        cer_str = f"{m['cer'] * 100:.0f}%" if m["cer"] is not None else "-"
        comp_lines.append(
            f"{i + 1:>3} | {det_time:<26} | {det_text:<40} | "
            f"{real_time:<26} | {real_text:<40} | {tm:>4} | {cer_str:>5}"
        )

    comp_lines.append("")
    comp_lines.append("说明:")
    comp_lines.append("  时间 ✓ = 时间重叠 ≥ 任一方时长 50%")
    comp_lines.append("  CER = 编辑距离 / 真实文本长度（仅时间匹配且非空时计算）")
    comp_lines.append("")

    out_comparison.write_text("\n".join(comp_lines) + "\n", encoding="utf-8")
    print(f"对比: {out_comparison}")


if __name__ == "__main__":
    main()
