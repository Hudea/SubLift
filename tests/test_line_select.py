"""feat-034c/d：行级选择与多帧共识。"""

from __future__ import annotations

import pytest

from sublift.models import (
    SCRIPT_AUTO,
    SCRIPT_CJK,
    BoundingBox,
    OcrLine,
    OcrResult,
    SubtitleProfile,
)
from sublift.pipeline.line_select import (
    cjk_ratio,
    cleanup_subtitle_text,
    consensus_text,
    edit_distance,
    score_line,
    select_line,
    should_accept_text,
)


def _line(
    text: str,
    conf: float,
    *,
    x: int = 100,
    y: int = 30,
    w: int = 200,
    h: int = 40,
) -> OcrLine:
    return OcrLine(
        text=text,
        confidence=conf,
        box=BoundingBox(x=x, y=y, width=w, height=h),
    )


def _profile() -> SubtitleProfile:
    return SubtitleProfile(
        script="cjk",
        center_x=500,
        center_y=40,
        height=40,
        y_min=10,
        y_max=70,
    )


class TestScriptAndSelect:
    def test_cjk_ratio(self) -> None:
        assert cjk_ratio("你好世界") == 1.0
        assert cjk_ratio("Hello") == 0.0
        assert cjk_ratio("Hi你好") == 0.5

    def test_selects_chinese_over_english_banner(self) -> None:
        profile = _profile()
        lines = [
            _line("BREAKING NEWS TICKER", 0.95, y=5, h=20),  # 顶部英文
            _line("你好，尼克", 0.32, y=35, h=40, x=400),  # 目标字幕低 conf
            _line("John Doe", 0.9, y=60, h=18, x=50),  # 姓名
        ]
        chosen = select_line(lines, profile)
        assert chosen is not None
        assert "你好" in chosen.text

    def test_rejects_english_only_when_cjk_profile(self) -> None:
        profile = _profile()
        lines = [
            _line("ONLY ENGLISH HERE", 0.99, y=35, h=40),
        ]
        chosen = select_line(lines, profile, min_script=0.12)
        assert chosen is None

    def test_score_prefers_y_band(self) -> None:
        profile = _profile()
        in_band = _line("中文", 0.5, y=30, h=40)
        out_band = _line("中文", 0.5, y=200, h=40)
        assert score_line(in_band, profile).total > score_line(out_band, profile).total


class TestConsensus:
    def test_majority(self) -> None:
        result = consensus_text(
            [
                ("你好", 0.3),
                ("你好", 0.35),
                ("你好啊", 0.9),
            ],
            script=SCRIPT_CJK,
        )
        assert result.text == "你好"
        assert result.confidence == pytest.approx(1.55 / 3)
        assert result.support_votes == 3

    def test_medoid_on_tie(self) -> None:
        result = consensus_text(
            [
                ("你好", 0.5),
                ("你好啊", 0.5),
            ],
            script=SCRIPT_CJK,
        )
        # 平票时 medoid；两者互为距离 1，稳定选字典序或 cost 相同的第一个
        assert result.text in {"你好", "你好啊"}

    def test_cjk_variants_share_votes_and_drop_unstable_latin_edges(self) -> None:
        result = consensus_text(
            [
                ("NEWS（动物方城市新闻台）ABCD", 0.30),
                ("LIVE（动物方城市新闻台）EF", 0.32),
                ("（动物方城市新闻台）", 0.31),
            ],
            script=SCRIPT_CJK,
        )
        assert result.text == "（动物方城市新闻台）"
        assert result.confidence == pytest.approx(0.31)
        assert result.support_votes == 3

    def test_stable_mixed_text_is_preserved(self) -> None:
        result = consensus_text(
            [("任务代号 FOX TWO", 0.4), ("任务代号 FOX TWO", 0.42)],
            script=SCRIPT_CJK,
        )
        assert result.text == "任务代号 FOX TWO"
        assert result.support_votes == 2

    def test_auto_does_not_fold_unrelated_english_into_cjk(self) -> None:
        result = consensus_text(
            [("HELLO", 0.9), ("你好", 0.4)],
            script=SCRIPT_AUTO,
        )
        assert result.text == "HELLO"
        assert result.support_votes == 1

    def test_edit_distance(self) -> None:
        assert edit_distance("你好", "你好") == 0
        assert edit_distance("你好", "你好啊") == 1


class TestAcceptPolicy:
    def test_high_conf_cjk_accepted(self) -> None:
        assert should_accept_text(
            "你好",
            0.9,
            profile=_profile(),
            confidence_threshold=0.5,
            low_conf_threshold=0.28,
            support_votes=1,
        )

    def test_low_conf_single_frame_rejected(self) -> None:
        assert not should_accept_text(
            "你好",
            0.3,
            profile=_profile(),
            confidence_threshold=0.5,
            low_conf_threshold=0.28,
            support_votes=1,
        )

    def test_low_conf_stable_multi_frame_accepted(self) -> None:
        assert should_accept_text(
            "你好",
            0.3,
            profile=_profile(),
            confidence_threshold=0.5,
            low_conf_threshold=0.28,
            support_votes=2,
        )

    def test_high_conf_english_noise_rejected_for_cjk(self) -> None:
        assert not should_accept_text(
            "ONLY ENGLISH NEWS",
            0.99,
            profile=_profile(),
            confidence_threshold=0.5,
            low_conf_threshold=0.28,
            support_votes=3,
        )


class TestCleanup:
    def test_keep_legitimate_english(self) -> None:
        assert cleanup_subtitle_text("HELLO") == "HELLO"
        assert cleanup_subtitle_text("欢迎来到 ZPD") == "欢迎来到 ZPD"
        assert cleanup_subtitle_text("任务代号 FOX TWO") == "任务代号 FOX TWO"
        assert cleanup_subtitle_text("后来加入ZPD警局") == "后来加入ZPD警局"

    def test_strip_only_latin_attached_to_cjk_edges(self) -> None:
        assert cleanup_subtitle_text("（前市长杨咩咩入狱）PHISON") == ("（前市长杨咩咩入狱）")
        assert cleanup_subtitle_text("在动物方城市气候墙SON") == ("在动物方城市气候墙")
        assert cleanup_subtitle_text("FOR一起揭穿阴谋SON") == "一起揭穿阴谋"
        assert cleanup_subtitle_text("哈茱蒂，本市第一位兔警员EFS CONSPI _N/、") == (
            "哈茱蒂，本市第一位兔警员"
        )

    def test_normalize_ellipsis_and_quotes(self) -> None:
        assert cleanup_subtitle_text("（前情提要⋯）") == "（前情提要…）"
        assert cleanup_subtitle_text("他们说：『没问题』") == "他们说：「没问题」"
        assert cleanup_subtitle_text("事实上.") == "事实上…"


class TestOcrResultLinesIntegration:
    def test_from_lines_then_select(self) -> None:
        lines = (
            _line("ENGLISH", 0.9, y=0),
            _line("字幕", 0.4, y=35),
        )
        result = OcrResult.from_lines(lines)
        chosen = select_line(result.lines, _profile())
        assert chosen is not None
        assert chosen.text == "字幕"
