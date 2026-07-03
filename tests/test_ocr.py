"""OCR 引擎模块（ocr）测试。

Mock 单测默认运行；Vision 单测需 PyObjC，集成测试需 macOS + Vision。
"""

from __future__ import annotations

import pytest
from PIL import Image, ImageDraw, ImageFont

from sublift.models import OcrResult
from sublift.ocr.base import OcrEngine
from sublift.ocr.mock import MockOcrEngine
from sublift.ocr.vision import VisionOcrEngine, is_vision_available


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

    def test_recognize_empty_image(self) -> None:
        """纯色空白图应返回空文本。"""
        img = Image.new("RGB", (200, 100), "white")
        engine = VisionOcrEngine()
        result = engine.recognize(img)
        assert result.text == ""
        assert result.confidence == 0.0
