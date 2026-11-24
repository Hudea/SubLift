#!/usr/bin/env python3
"""authoritative SRT evaluation utility: Time IoU matching + CER + timing offsets.

Usage:
    python3 scripts/srt_eval.py <ground_truth.srt> <prediction.srt> [prediction2.srt]
    python3 scripts/srt_eval.py <ground_truth.srt> <prediction.srt> --json
    python3 scripts/srt_eval.py <ground_truth.srt> <prediction.srt> --assert-f1 0.900 --assert-cer 0.085
"""

import argparse
import json
import re
import statistics
import sys

TS_REGEX = re.compile(r"(\d+):(\d+):(\d+)[,.](\d+)\s*-->\s*(\d+):(\d+):(\d+)[,.](\d+)")
PUNCT_REGEX = re.compile(r"[\s，。、！？…,.!?（）()“”\"'：:；;\-—~·《》〈〉【】〔〕]")


def parse_srt(path):
    """Parses an SRT file into a list of tuples: (start_ms, end_ms, text)."""
    cues = []
    with open(path, mode="r", encoding="utf-8-sig", errors="replace") as f:
        content = f.read()

    blocks = re.split(r"\r?\n\s*\r?\n", content.strip())
    for b in blocks:
        lines = [line.strip() for line in b.splitlines() if line.strip()]
        for idx, line in enumerate(lines):
            match = TS_REGEX.search(line)
            if match:
                g = list(map(int, match.groups()))
                start_ms = ((g[0] * 60 + g[1]) * 60 + g[2]) * 1000 + g[3]
                end_ms = ((g[4] * 60 + g[5]) * 60 + g[6]) * 1000 + g[7]
                text = "\n".join(lines[idx + 1:])
                cues.append((start_ms, end_ms, text))
                break
    return cues


def norm_text(text):
    """Strips all whitespace and standard CJK/ASCII punctuation."""
    return PUNCT_REGEX.sub("", text)


def levenshtein_distance(seq_a, seq_b):
    """Computes the Levenshtein edit distance between two sequences."""
    if not seq_a:
        return len(seq_b)
    prev = list(range(len(seq_b) + 1))
    for i, ca in enumerate(seq_a, 1):
        curr = [i]
        for j, cb in enumerate(seq_b, 1):
            curr.append(min(prev[j] + 1, curr[j - 1] + 1, prev[j - 1] + (ca != cb)))
        prev = curr
    return prev[-1]


def compute_iou(interval_a, interval_b):
    """Computes temporal Intersection over Union between two (start, end) intervals."""
    inter = max(0, min(interval_a[1], interval_b[1]) - max(interval_a[0], interval_b[0]))
    union = max(interval_a[1], interval_b[1]) - min(interval_a[0], interval_b[0])
    return inter / union if union > 0 else 0.0


def evaluate_cues(gt_cues, pred_cues, thr=0.3):
    """Evaluates predicted cues against ground truth cues using greedy IoU matching."""
    used_pred_indices = set()
    matches = []

    for gi, gt in enumerate(gt_cues):
        best_iou, best_pj = 0.0, -1
        for pj, pred in enumerate(pred_cues):
            if pj in used_pred_indices:
                continue
            val = compute_iou(gt, pred)
            if val > best_iou:
                best_iou, best_pj = val, pj
        if best_iou >= thr:
            used_pred_indices.add(best_pj)
            matches.append((gi, best_pj, best_iou))

    tp = len(matches)
    precision = tp / len(pred_cues) if pred_cues else 0.0
    recall = tp / len(gt_cues) if gt_cues else 0.0
    f1 = (2.0 * precision * recall / (precision + recall)) if (precision + recall) > 0.0 else 0.0

    errs = sum(levenshtein_distance(norm_text(gt_cues[g][2]), norm_text(pred_cues[p][2])) for g, p, _ in matches)
    chars = sum(len(norm_text(gt_cues[g][2])) for g, _, _ in matches) or 1
    exact_text_matches = sum(1 for g, p, _ in matches if norm_text(gt_cues[g][2]) == norm_text(pred_cues[p][2]))

    if len(matches) == 0 and len(gt_cues) > 0:
        cer_matched = 1.0
    else:
        cer_matched = round(errs / chars, 4)

    delta_start = [pred_cues[p][0] - gt_cues[g][0] for g, p, _ in matches]
    delta_end = [pred_cues[p][1] - gt_cues[g][1] for g, p, _ in matches]

    abs_delta_start = sorted(abs(d) for d in delta_start)
    p90_idx = max(0, int(len(abs_delta_start) * 0.9) - 1) if abs_delta_start else 0
    abs_delta_end = sorted(abs(d) for d in delta_end)
    end_p90_idx = max(0, int(len(abs_delta_end) * 0.9) - 1) if abs_delta_end else 0

    return {
        "gt": len(gt_cues),
        "pred": len(pred_cues),
        "tp": tp,
        "precision": round(precision, 3),
        "recall": round(recall, 3),
        "f1": round(f1, 3),
        "cer_matched": cer_matched,
        "exact_text": exact_text_matches,
        "start_off_med": round(statistics.median(delta_start), 1) if delta_start else None,
        "end_off_med": round(statistics.median(delta_end), 1) if delta_end else None,
        "abs_start_p90": abs_delta_start[p90_idx] if abs_delta_start else None,
        "abs_end_p90": abs_delta_end[end_p90_idx] if abs_delta_end else None,
    }


def diff_cues(cues_a, cues_b):
    """Returns count of cues unique to A and unique to B."""
    sa = {(s, e, t) for s, e, t in cues_a}
    sb = {(s, e, t) for s, e, t in cues_b}
    return len(sa - sb), len(sb - sa)


def main():
    parser = argparse.ArgumentParser(description="SRT Evaluation: Time IoU matching + CER + timing offsets")
    parser.add_argument("gt_srt", help="Path to ground truth SRT file")
    parser.add_argument("pred_srt", help="Path to predicted SRT file")
    parser.add_argument("pred2_srt", nargs="?", default=None, help="Optional second predicted SRT file for diffing")
    parser.add_argument("--thr", type=float, default=0.3, help="IoU match threshold (default: 0.3)")
    parser.add_argument(
        "--time-range-ms",
        default=None,
        help="Optional START:END millisecond window to filter both GT and pred cues",
    )
    parser.add_argument("--json", action="store_true", help="Output results in JSON format")
    parser.add_argument("--assert-f1", type=float, default=None, help="Assert F1 >= threshold, exit 1 on failure")
    parser.add_argument("--assert-cer", type=float, default=None, help="Assert CER <= threshold, exit 1 on failure")
    parser.add_argument("--assert-cues", type=int, default=None, help="Assert pred cue count == expected, exit 1 on failure")

    args = parser.parse_args()

    gt_cues = parse_srt(args.gt_srt)
    pred_cues = parse_srt(args.pred_srt)
    if args.time_range_ms is not None:
        parts = args.time_range_ms.split(":")
        if len(parts) != 2:
            parser.error("--time-range-ms must be in START:END format")
        start_ms, end_ms = int(parts[0]), int(parts[1])
        gt_cues = [c for c in gt_cues if c[1] > start_ms and c[0] < end_ms]
        pred_cues = [c for c in pred_cues if c[1] > start_ms and c[0] < end_ms]

    res1 = evaluate_cues(gt_cues, pred_cues, thr=args.thr)

    res2 = None
    diff_res = None
    if args.pred2_srt:
        p2_cues = parse_srt(args.pred2_srt)
        if args.time_range_ms is not None:
            p2_cues = [c for c in p2_cues if c[1] > start_ms and c[0] < end_ms]
        res2 = evaluate_cues(gt_cues, p2_cues, thr=args.thr)
        diff_res = {
            "identical": pred_cues == p2_cues,
            "diff_only_in_1": diff_cues(pred_cues, p2_cues)[0],
            "diff_only_in_2": diff_cues(pred_cues, p2_cues)[1],
        }

    if args.json:
        payload = {"eval": res1}
        if res2 is not None:
            payload["eval2"] = res2
            payload["comparison"] = diff_res
        print(json.dumps(payload, ensure_ascii=False, indent=2))
    else:
        print("eval:", res1)
        if res2 is not None:
            print("eval2:", res2)
            print("identical:", diff_res["identical"],
                  "diff(only_in_1, only_in_2):",
                  (diff_res["diff_only_in_1"], diff_res["diff_only_in_2"]))

    # Assertion checks
    failed = False
    if args.assert_f1 is not None and res1["f1"] < args.assert_f1:
        sys.stderr.write(f"Assertion failed: F1 ({res1['f1']}) < expected ({args.assert_f1})\n")
        failed = True
    if args.assert_cer is not None and res1["cer_matched"] > args.assert_cer:
        sys.stderr.write(f"Assertion failed: CER ({res1['cer_matched']}) > expected ({args.assert_cer})\n")
        failed = True
    if args.assert_cues is not None and res1["pred"] != args.assert_cues:
        sys.stderr.write(f"Assertion failed: Cue count ({res1['pred']}) != expected ({args.assert_cues})\n")
        failed = True

    if failed:
        sys.exit(1)


if __name__ == "__main__":
    main()
