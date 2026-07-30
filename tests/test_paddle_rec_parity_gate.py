from __future__ import annotations

from scripts.parity.check_paddle_rec_parity import (
    _classification_metrics,
    _decode_metrics,
    _final_metrics,
)


def test_classification_metrics_compare_label_and_score() -> None:
    result = _classification_metrics(
        [{"label": "180", "score": 0.9999}],
        [{"label": "180", "score": 0.9998, "rotated_180": True}],
    )

    assert result["count_equal"] is True
    assert result["labels_exact"] is True
    assert result["score_max_abs"] < 1.1e-4


def test_decode_metrics_require_tokens_text_and_confidence() -> None:
    result = _decode_metrics(
        [{"tokens": [1, 2], "text": "AB", "confidence": 0.9}],
        [{"tokens": [1, 2], "text": "AB", "confidence": 0.90001}],
    )

    assert result["count_equal"] is True
    assert result["tokens_exact"] is True
    assert result["text_exact"] is True
    assert result["confidence_max_abs"] < 1.1e-5


def test_final_metrics_report_order_and_aabb_error() -> None:
    oracle = {
        "text": "first\nsecond",
        "lines": [
            {
                "text": "first",
                "box": {"x": 1, "y": 2, "width": 30, "height": 10},
            },
            {
                "text": "second",
                "box": {"x": 2, "y": 20, "width": 40, "height": 11},
            },
        ],
    }
    candidate = {
        "text": "first\nsecond",
        "lines": [
            {
                "text": "first",
                "box": {"x": 2, "y": 2, "width": 30, "height": 10},
            },
            {
                "text": "second",
                "box": {"x": 2, "y": 20, "width": 40, "height": 11},
            },
        ],
    }

    result = _final_metrics(oracle, candidate)
    assert result == {
        "line_count_equal": True,
        "text_exact": True,
        "order_exact": True,
        "aabb_max_abs": 1,
    }
