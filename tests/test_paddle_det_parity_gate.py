from __future__ import annotations

from typing import Any

import pytest
from scripts.parity.check_paddle_det_parity import _match_quads


def _quad(
    x: float,
    y: float,
    width: float,
    height: float,
    *,
    score: float = 0.9,
) -> dict[str, Any]:
    return {
        "points": [
            [x, y],
            [x + width, y],
            [x + width, y + height],
            [x, y + height],
        ],
        "score": score,
    }


def test_match_quads_reports_exact_one_to_one_metrics() -> None:
    result = _match_quads(
        [_quad(10, 20, 100, 30, score=0.9)],
        [_quad(10, 20, 100, 30, score=0.90001)],
    )

    assert result["precision_at_iou_0_95"] == 1.0
    assert result["recall_at_iou_0_95"] == 1.0
    assert result["match_count"] == 1
    assert result["matches"][0]["iou"] == pytest.approx(1.0)
    assert result["matches"][0]["coord_max_abs"] == 0.0
    assert result["matches"][0]["score_abs"] == pytest.approx(1e-5)


def test_match_quads_rejects_non_overlapping_candidate() -> None:
    result = _match_quads(
        [_quad(0, 0, 50, 20)],
        [_quad(100, 100, 50, 20)],
    )

    assert result["match_count"] == 0
    assert result["precision_at_iou_0_95"] == 0.0
    assert result["recall_at_iou_0_95"] == 0.0
