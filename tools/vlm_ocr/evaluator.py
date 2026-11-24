"""SRT evaluation wrapper delegating strictly to scripts/srt_eval.py."""

from __future__ import annotations

import importlib.util
from pathlib import Path
from types import ModuleType
from typing import Any


_SRT_EVAL_CACHE: ModuleType | None = None


def load_authoritative_srt_eval(repo_root: Path | None = None) -> ModuleType:
    """Dynamically load the repository's single authoritative scripts/srt_eval.py."""
    global _SRT_EVAL_CACHE
    if _SRT_EVAL_CACHE is not None:
        return _SRT_EVAL_CACHE

    root = repo_root or Path(__file__).resolve().parents[2]
    eval_path = root / "scripts" / "srt_eval.py"
    if not eval_path.is_file():
        raise FileNotFoundError(f"Authoritative evaluator not found: {eval_path}")

    spec = importlib.util.spec_from_file_location("sublift_authoritative_srt_eval", eval_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Unable to load module spec from {eval_path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    _SRT_EVAL_CACHE = module
    return module


def _collect_text_diffs(
    srt_eval: ModuleType,
    gt_cues: list[tuple[int, int, str]],
    pred_cues: list[tuple[int, int, str]],
    thr: float,
    max_diffs: int = 30,
) -> list[dict[str, Any]]:
    """Collect up to `max_diffs` matched cue text mismatches for diagnostics."""
    used_pred: set[int] = set()
    diffs: list[dict[str, Any]] = []

    for gi, gt in enumerate(gt_cues):
        best_iou = 0.0
        best_pj = -1
        for pj, pred in enumerate(pred_cues):
            if pj in used_pred:
                continue
            iou = srt_eval.compute_iou(gt, pred)
            if iou > best_iou:
                best_iou = iou
                best_pj = pj
        if best_iou < thr or best_pj < 0:
            continue

        used_pred.add(best_pj)
        gt_text = gt[2]
        pred_text = pred_cues[best_pj][2]
        if srt_eval.norm_text(gt_text) == srt_eval.norm_text(pred_text):
            continue

        if len(diffs) < max_diffs:
            diffs.append(
                {
                    "gt_index": gi + 1,
                    "pred_index": best_pj + 1,
                    "iou": round(best_iou, 4),
                    "start_ms": gt[0],
                    "end_ms": gt[1],
                    "gt_text": gt_text,
                    "pred_text": pred_text,
                    "norm_edit_distance": srt_eval.levenshtein_distance(
                        srt_eval.norm_text(gt_text), srt_eval.norm_text(pred_text)
                    ),
                }
            )
    return diffs


def evaluate_srt_pair(
    reference_srt: Path,
    candidate_srt: Path,
    *,
    iou_thr: float = 0.5,
    max_diffs: int = 30,
    repo_root: Path | None = None,
) -> dict[str, Any]:
    """Evaluate `candidate_srt` against `reference_srt` at both `iou_thr` and 0.3."""
    srt_eval = load_authoritative_srt_eval(repo_root)
    ref_cues = srt_eval.parse_srt(str(reference_srt))
    cand_cues = srt_eval.parse_srt(str(candidate_srt))

    primary = srt_eval.evaluate_cues(ref_cues, cand_cues, thr=iou_thr)
    baseline_03 = (
        primary
        if abs(iou_thr - 0.3) < 1e-9
        else srt_eval.evaluate_cues(ref_cues, cand_cues, thr=0.3)
    )
    diffs = _collect_text_diffs(srt_eval, ref_cues, cand_cues, thr=iou_thr, max_diffs=max_diffs)

    return {
        "reference_srt": str(reference_srt),
        "candidate_srt": str(candidate_srt),
        "primary_iou_thr": iou_thr,
        "primary": primary,
        "iou_0_3": baseline_03,
        "text_diffs": diffs,
    }
