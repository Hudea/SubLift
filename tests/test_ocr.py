"""OCR 引擎模块（ocr）测试。

Mock 单测默认运行；Vision 单测需 PyObjC，集成测试需 macOS + Vision。
"""

from __future__ import annotations

import pytest
from PIL import Image, ImageDraw, ImageFont

from sublift.models import BoundingBox, OcrLine, OcrResult, PersistentTextPolicy, SubtitleProfile
from sublift.ocr.base import OcrEngine
from sublift.ocr.mock import MockOcrEngine
from sublift.ocr.vision import VisionOcrEngine, is_vision_available


class TestOcrLineModel:
    """OcrLine / OcrResult.lines 数据模型测试（feat-033a）。"""

    def test_ocr_line_construction(self) -> None:
        bbox = BoundingBox(x=10, y=20, width=100, height=30)
        line = OcrLine(text="你好", confidence=0.9, bbox=bbox)
        assert line.text == "你好"
        assert line.confidence == 0.9
        assert line.bbox == bbox

    def test_ocr_result_default_lines_empty(self) -> None:
        """OcrResult 不传 lines 时默认空列表（向后兼容）。"""
        result = OcrResult(text="hello", confidence=0.8)
        assert result.lines == []

    def test_ocr_result_with_lines(self) -> None:
        bbox = BoundingBox(x=0, y=0, width=50, height=20)
        line = OcrLine(text="a", confidence=0.9, bbox=bbox)
        result = OcrResult(text="a", confidence=0.9, lines=[line])
        assert len(result.lines) == 1
        assert result.lines[0] == line

    def test_ocr_result_legacy_construction_still_works(self) -> None:
        """旧式构造 OcrResult(text, confidence) 必须仍然工作。"""
        result = OcrResult(text="hello", confidence=0.8)
        assert result.text == "hello"
        assert result.confidence == 0.8
        assert result.lines == []


class TestSubtitleProfileModel:
    """SubtitleProfile / PersistentTextPolicy 数据模型测试（feat-033b / feat-034a）。"""

    def test_profile_defaults(self) -> None:
        profile = SubtitleProfile(y_center=100.0, y_tolerance=20.0, line_height=40.0)
        assert profile.max_lines == 1
        assert profile.script_hint == "auto"
        assert profile.persistent_text_policy is None

    def test_profile_from_dict_without_policy(self) -> None:
        profile = SubtitleProfile.from_dict({
            "y_center": 100.0, "y_tolerance": 20.0, "line_height": 40.0,
        })
        assert profile.persistent_text_policy is None

    def test_profile_from_dict_with_policy(self) -> None:
        profile = SubtitleProfile.from_dict({
            "y_center": 100.0, "y_tolerance": 20.0, "line_height": 40.0,
            "persistent_text_policy": {
                "enabled": True, "min_repeat_segments": 2,
                "min_distinct_texts": 5, "y_bin_ratio": 0.4,
            },
        })
        assert profile.persistent_text_policy is not None
        assert profile.persistent_text_policy.enabled is True
        assert profile.persistent_text_policy.min_repeat_segments == 2
        assert profile.persistent_text_policy.min_distinct_texts == 5
        assert abs(profile.persistent_text_policy.y_bin_ratio - 0.4) < 1e-6

    def test_profile_from_dict_policy_non_dict_returns_none(self) -> None:
        """persistent_text_policy 不是 dict 时被视为 None（保守）。"""
        profile = SubtitleProfile.from_dict({
            "y_center": 100.0, "y_tolerance": 20.0, "line_height": 40.0,
            "persistent_text_policy": None,
        })
        assert profile.persistent_text_policy is None

    def test_persistent_policy_defaults(self) -> None:
        policy = PersistentTextPolicy()
        assert policy.enabled is True
        assert policy.min_repeat_segments == 3
        assert policy.min_distinct_texts == 4
        assert abs(policy.y_bin_ratio - 0.5) < 1e-6


class TestMockOcrEngine:
    """MockOcrEngine 单测，不依赖外部资源。"""

    def test_is_ocr_engine(self) -> None:
        engine = MockOcrEngine()
        assert isinstance(engine, OcrEngine)

    def test_fixed_mode_default(self) -> None:
        engine = MockOcrEngine()
        img = Image.new("RGB", (10, 10))
        result = engine.recognize(img)
        assert result == OcrResult(text="", confidence=1.0)

    def test_fixed_mode_custom(self) -> None:
        engine = MockOcrEngine(text="hello", confidence=0.9)
        img = Image.new("RGB", (10, 10))
        result = engine.recognize(img)
        assert result == OcrResult(text="hello", confidence=0.9)

    def test_fixed_mode_repeated(self) -> None:
        """固定模式多次调用返回相同结果。"""
        engine = MockOcrEngine(text="hello", confidence=0.9)
        img = Image.new("RGB", (10, 10))
        first = engine.recognize(img)
        second = engine.recognize(img)
        assert first == second == OcrResult(text="hello", confidence=0.9)

    def test_sequence_mode(self) -> None:
        """序列模式按调用顺序返回。"""
        seq = [
            OcrResult(text="a", confidence=0.9),
            OcrResult(text="b", confidence=0.8),
        ]
        engine = MockOcrEngine(sequence=seq)
        img = Image.new("RGB", (10, 10))
        assert engine.recognize(img) == OcrResult(text="a", confidence=0.9)
        assert engine.recognize(img) == OcrResult(text="b", confidence=0.8)

    def test_sequence_overflow_raises(self) -> None:
        """序列越界抛 IndexError。"""
        engine = MockOcrEngine(sequence=[OcrResult(text="a", confidence=0.9)])
        img = Image.new("RGB", (10, 10))
        engine.recognize(img)
        with pytest.raises(IndexError):
            engine.recognize(img)


@pytest.mark.skipif(not is_vision_available(), reason="PyObjC Vision 未安装")
class TestVisionOcrEngineUnit:
    """VisionOcrEngine 单测，需 PyObjC。"""

    def test_is_ocr_engine(self) -> None:
        engine = VisionOcrEngine()
        assert isinstance(engine, OcrEngine)


class TestVisionDegradation:
    """Vision 不可用时的降级测试，monkeypatch 模拟。"""

    def test_unavailable_raises(self, monkeypatch: pytest.MonkeyPatch) -> None:
        """_VISION_AVAILABLE 为 False 时实例化抛 RuntimeError。"""
        import sublift.ocr.vision as vision_mod

        monkeypatch.setattr(vision_mod, "_VISION_AVAILABLE", False)
        with pytest.raises(RuntimeError, match="Apple Vision 不可用"):
            VisionOcrEngine()


def _make_text_image(
    text: str,
    width: int = 400,
    height: int = 100,
    font_path: str = "/System/Library/Fonts/Helvetica.ttc",
    font_size: int = 40,
) -> Image.Image:
    """造一张带文字的图像用于 Vision 集成测试。"""
    img = Image.new("RGB", (width, height), "white")
    draw = ImageDraw.Draw(img)
    font = ImageFont.truetype(font_path, font_size)
    draw.text((20, 20), text, fill="black", font=font)
    return img


def _make_mixed_image(
    target_text: str,
    bg_text: str,
    width: int = 500,
    height: int = 200,
    target_y: int = 120,
    bg_y: int = 20,
    target_font_size: int = 40,
    bg_font_size: int = 25,
) -> Image.Image:
    """造一张含目标字幕 + 背景文字的图（feat-033e）。

    目标字幕在下方（target_y），背景英文在上方（bg_y）。
    用于验证 selector 能根据 profile 只选目标行。
    """
    img = Image.new("RGB", (width, height), "white")
    draw = ImageDraw.Draw(img)
    bg_font = ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", bg_font_size)
    draw.text((20, bg_y), bg_text, fill="gray", font=bg_font)
    target_font = ImageFont.truetype(
        "/System/Library/Fonts/STHeiti Light.ttc", target_font_size
    )
    draw.text((20, target_y), target_text, fill="black", font=target_font)
    return img


@pytest.mark.integration
@pytest.mark.skipif(not is_vision_available(), reason="PyObjC Vision 未安装")
class TestVisionOcrEngineIntegration:
    """VisionOcrEngine 真实识别集成测试，需 macOS + PyObjC。"""

    def test_recognize_hello(self) -> None:
        """Vision 应识别出 'Hello' 英文文本。"""
        img = _make_text_image("Hello")
        engine = VisionOcrEngine()
        result = engine.recognize(img)
        assert result.text != ""
        assert "hello" in result.text.lower()
        assert 0.0 <= result.confidence <= 1.0
        assert len(result.lines) >= 1
        assert result.lines[0].text != ""
        assert result.lines[0].confidence > 0.0
        assert result.lines[0].bbox.width > 0
        assert result.lines[0].bbox.height > 0

    def test_recognize_chinese(self) -> None:
        """Vision 默认启用 zh-Hans，应识别出中文文本。"""
        img = _make_text_image(
            "你好世界",
            width=500,
            height=120,
            font_path="/System/Library/Fonts/STHeiti Light.ttc",
            font_size=50,
        )
        engine = VisionOcrEngine()
        result = engine.recognize(img)
        assert result.text != ""
        assert "你好世界" in result.text
        assert 0.0 <= result.confidence <= 1.0
        assert len(result.lines) >= 1
        assert result.lines[0].bbox.width > 0
        assert result.lines[0].bbox.height > 0

    def test_recognize_empty_image(self) -> None:
        """纯色空白图应返回空文本。"""
        img = Image.new("RGB", (200, 100), "white")
        engine = VisionOcrEngine()
        result = engine.recognize(img)
        assert result.text == ""
        assert result.confidence == 0.0
        assert result.lines == []

    def test_recognize_bbox_within_image_bounds(self) -> None:
        """bbox 必须落在裁剪图范围内（feat-033a 坐标转换正确性）。"""
        img = _make_text_image("Test", width=400, height=100)
        engine = VisionOcrEngine()
        result = engine.recognize(img)
        if result.lines:
            for line in result.lines:
                assert 0 <= line.bbox.x < 400
                assert 0 <= line.bbox.y < 100
                assert line.bbox.x + line.bbox.width <= 400
                assert line.bbox.y + line.bbox.height <= 100


@pytest.mark.integration
@pytest.mark.skipif(not is_vision_available(), reason="PyObjC Vision 未安装")
class TestSelectorWithVision:
    """feat-033e：selector + VisionOcrEngine 端到端验证。

    用 PIL 合成含目标中文 + 背景英文的图，Vision OCR 识别后
    selector 根据 profile 只选目标字幕行。
    """

    def test_selector_filters_background_english(self) -> None:
        """目标中文字幕在下方，背景英文在上方，selector 只选中文字幕。"""
        img = _make_mixed_image(
            target_text="你好世界",
            bg_text="CHAPTER ONE",
            width=500,
            height=200,
            target_y=120,
            bg_y=20,
            target_font_size=40,
            bg_font_size=25,
        )
        engine = VisionOcrEngine()
        result = engine.recognize(img)

        assert len(result.lines) >= 2, "Vision 应至少识别出 2 行"

        # 目标字幕在下方（y > 100），背景在上方（y < 100）
        target_lines = [ln for ln in result.lines if ln.bbox.y > 100]
        bg_lines = [ln for ln in result.lines if ln.bbox.y < 100]
        assert len(target_lines) >= 1, "应有目标字幕行"
        assert len(bg_lines) >= 1, "应有背景英文行"

        # 用目标行的 y 中心构造 profile
        target = target_lines[0]
        y_center = target.bbox.y + target.bbox.height / 2.0
        line_height = target.bbox.height

        from sublift.models import SubtitleProfile
        from sublift.ocr.selector import select_lines

        profile = SubtitleProfile(
            y_center=y_center,
            y_tolerance=line_height / 2.0,
            line_height=line_height,
            max_lines=2,
        )
        selected_text, _ = select_lines(result.lines, profile)

        assert "你好世界" in selected_text
        assert "CHAPTER" not in selected_text.upper()

    def test_selector_no_profile_keeps_all(self) -> None:
        """无 profile 时 OcrResult.text 包含所有行（旧路径兼容）。"""
        img = _make_mixed_image(
            target_text="你好",
            bg_text="TITLE",
            width=400,
            height=200,
            target_y=120,
            bg_y=20,
        )
        engine = VisionOcrEngine()
        result = engine.recognize(img)

        assert "你好" in result.text
        assert "TITLE" in result.text.upper()
