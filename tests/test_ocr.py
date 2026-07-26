"""OCR 引擎模块（ocr）测试。

Mock 单测默认运行；Vision 单测需 PyObjC，集成测试需 macOS + Vision。
PaddleOcrEngine 单测需 rapidocr，集成测试需模型已下载。
"""

from __future__ import annotations

from types import SimpleNamespace

import numpy as np
import pytest
from PIL import Image, ImageDraw, ImageFont

from sublift.models import BoundingBox, OcrLine, OcrResult
from sublift.ocr.base import OcrEngine
from sublift.ocr.mock import MockOcrEngine
from sublift.ocr.paddle import PaddleOcrEngine, is_paddle_available
from sublift.ocr.vision import (
    VisionOcrEngine,
    _collect_results,
    _vision_box_to_pixel,
    is_vision_available,
)


class TestOcrResultFromLines:
    """OcrResult.from_lines 兼容 join 约定。"""

    def test_empty(self) -> None:
        result = OcrResult.from_lines([])
        assert result == OcrResult(text="", confidence=0.0, lines=())

    def test_multiline_join_and_mean_conf(self) -> None:
        lines = (
            OcrLine(
                text="hello",
                confidence=0.8,
                box=BoundingBox(x=0, y=0, width=40, height=10),
            ),
            OcrLine(
                text="world",
                confidence=0.4,
                box=BoundingBox(x=0, y=20, width=40, height=10),
            ),
        )
        result = OcrResult.from_lines(lines)
        assert result.text == "hello\nworld"
        assert result.confidence == pytest.approx(0.6)
        assert result.lines == lines

    def test_legacy_constructor_empty_lines(self) -> None:
        result = OcrResult(text="hi", confidence=0.9)
        assert result.lines == ()
        assert result.text == "hi"


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

    def test_fixed_mode_with_lines(self) -> None:
        lines = [
            OcrLine(
                text="上",
                confidence=0.9,
                box=BoundingBox(x=1, y=2, width=10, height=8),
            ),
            OcrLine(
                text="下",
                confidence=0.7,
                box=BoundingBox(x=1, y=20, width=10, height=8),
            ),
        ]
        engine = MockOcrEngine(lines=lines)
        img = Image.new("RGB", (10, 10))
        result = engine.recognize(img)
        assert result.text == "上\n下"
        assert result.confidence == pytest.approx(0.8)
        assert result.lines == tuple(lines)

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


class TestVisionBoxToPixel:
    """Vision 归一化 box → 像素左上（纯函数，不依赖 PyObjC）。"""

    def test_full_frame_origin_bottom_left(self) -> None:
        # 整图：归一化 origin=(0,0) size=(1,1) → 像素 (0,0,W,H)
        box = _vision_box_to_pixel(
            SimpleNamespace(
                origin=SimpleNamespace(x=0.0, y=0.0),
                size=SimpleNamespace(width=1.0, height=1.0),
            ),
            200,
            100,
        )
        assert box == BoundingBox(x=0, y=0, width=200, height=100)

    def test_top_band(self) -> None:
        # 顶部半区：Vision y 从 0.5 起、高 0.5 → 像素 y=0,h=50
        box = _vision_box_to_pixel(
            SimpleNamespace(
                origin=SimpleNamespace(x=0.0, y=0.5),
                size=SimpleNamespace(width=1.0, height=0.5),
            ),
            100,
            100,
        )
        assert box.x == 0
        assert box.y == 0
        assert box.width == 100
        assert box.height == 50

    def test_tuple_form(self) -> None:
        box = _vision_box_to_pixel(((0.1, 0.2), (0.3, 0.4)), 100, 100)
        assert box.x == 10
        # y = (1 - 0.2 - 0.4) * 100 = 40
        assert box.y == 40
        assert box.width == 30
        assert box.height == 40


class TestCollectResults:
    """_collect_results 行级收集（假 observation，不依赖 Vision 运行时）。"""

    def test_multiline_sorted_by_y(self) -> None:
        def make_obs(
            text: str,
            conf: float,
            nx: float,
            ny: float,
            nw: float,
            nh: float,
        ) -> SimpleNamespace:
            return SimpleNamespace(
                topCandidates_=lambda _n: [
                    SimpleNamespace(string=lambda: text, confidence=lambda: conf)
                ],
                boundingBox=lambda: SimpleNamespace(
                    origin=SimpleNamespace(x=nx, y=ny),
                    size=SimpleNamespace(width=nw, height=nh),
                ),
            )

        # 故意乱序：下方行先出现
        request = SimpleNamespace(
            results=lambda: [
                make_obs("bottom", 0.5, 0.0, 0.0, 1.0, 0.3),
                make_obs("top", 0.9, 0.0, 0.7, 1.0, 0.3),
            ]
        )
        result = _collect_results(request, (100, 100))
        assert result.text == "top\nbottom"
        assert len(result.lines) == 2
        assert result.lines[0].text == "top"
        assert result.lines[1].text == "bottom"
        assert result.confidence == pytest.approx(0.7)
        assert result.lines[0].box.y < result.lines[1].box.y

    def test_skips_empty_text(self) -> None:
        request = SimpleNamespace(
            results=lambda: [
                SimpleNamespace(
                    topCandidates_=lambda _n: [
                        SimpleNamespace(string=lambda: "  ", confidence=lambda: 0.9)
                    ],
                    boundingBox=lambda: ((0.0, 0.0), (1.0, 1.0)),
                )
            ]
        )
        result = _collect_results(request, (50, 50))
        assert result == OcrResult(text="", confidence=0.0, lines=())

    def test_empty_observations(self) -> None:
        request = SimpleNamespace(results=lambda: [])
        result = _collect_results(request, (50, 50))
        assert result.lines == ()
        assert result.text == ""


@pytest.mark.skipif(not is_vision_available(), reason="PyObjC Vision 未安装")
class TestVisionOcrEngineUnit:
    """VisionOcrEngine 单测，需 PyObjC。"""

    def test_is_ocr_engine(self) -> None:
        engine = VisionOcrEngine()
        assert isinstance(engine, OcrEngine)

    def test_no_callback_uses_untimed_path(self) -> None:
        """无 timing_callback 时走 _recognize_untimed（产品快速路径）。"""
        engine = VisionOcrEngine()
        assert engine._timing_callback is None
        img = Image.new("RGB", (64, 32), "white")
        # 无 callback：recognize 与 untimed 返回一致
        result = engine.recognize(img)
        untimed = engine._recognize_untimed(img)
        assert result.text == untimed.text == ""
        assert result.confidence == untimed.confidence == 0.0

    def test_callback_exception_does_not_break_ocr(self) -> None:
        """callback 抛异常不得改变 OCR 返回语义。"""

        def _boom(_detail: object) -> None:
            raise RuntimeError("observer boom")

        engine = VisionOcrEngine(timing_callback=_boom)
        img = Image.new("RGB", (64, 32), "white")
        result = engine.recognize(img)
        # 空白图仍返回空结果，不被 observer 异常污染
        assert result.text == ""
        assert result.confidence == 0.0

    def test_callback_receives_detail_on_success_or_empty(self) -> None:
        """有 callback 时每次调用产出完整五阶段明细。"""
        from sublift.diagnostics.performance import OcrCallDetail

        seen: list[OcrCallDetail] = []

        def _collect(detail: OcrCallDetail) -> None:
            seen.append(detail)

        engine = VisionOcrEngine(timing_callback=_collect)
        img = Image.new("RGB", (64, 32), "white")
        engine.recognize(img)
        assert len(seen) == 1
        detail = seen[0]
        assert detail.input_width == 64
        assert detail.input_height == 32
        assert detail.outcome in {"success", "empty", "error"}
        # 五阶段字段存在且非负
        assert detail.input_prepare_ms >= 0.0
        assert detail.request_setup_ms >= 0.0
        assert detail.vision_perform_ms >= 0.0
        assert detail.observation_mapping_ms >= 0.0
        assert detail.residual_ms >= 0.0
        assert detail.total_ms >= 0.0


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


def _make_two_line_image() -> Image.Image:
    """上下两行英文，用于行级 lines 集成测。"""
    img = Image.new("RGB", (400, 160), "white")
    draw = ImageDraw.Draw(img)
    font = ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", 36)
    draw.text((20, 20), "Hello", fill="black", font=font)
    draw.text((20, 90), "World", fill="black", font=font)
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
        assert result.lines == ()

    def test_recognize_multiline_lines_populated(self) -> None:
        """多行图应填充 lines，每行含 conf 与合法 box。"""
        img = _make_two_line_image()
        engine = VisionOcrEngine()
        result = engine.recognize(img)
        assert result.text != ""
        assert len(result.lines) >= 1
        for line in result.lines:
            assert line.text.strip()
            assert 0.0 <= line.confidence <= 1.0
            assert line.box.width >= 0
            assert line.box.height >= 0
            assert line.box.x >= 0
            assert line.box.y >= 0
        # 兼容汇总应与 lines 一致
        assert result.text == "\n".join(line.text for line in result.lines)


@pytest.mark.skipif(not is_paddle_available(), reason="rapidocr 未安装")
class TestPaddleOcrEngineUnit:
    """PaddleOcrEngine 单测，不触发模型下载。"""

    def test_default_model_dir(self) -> None:
        from pathlib import Path

        from sublift.ocr.paddle import DEFAULT_MODEL_DIR

        expected = Path.home() / ".cache" / "sublift" / "rapidocr-models"
        assert str(DEFAULT_MODEL_DIR) == str(expected)


class TestPaddleRecognizeContract:
    """recognize() 映射逻辑单测，用假 RapidOCR 注入，不下载模型。

    覆盖：四角点 box -> BoundingBox 包围盒、行序排序、boxes=None 降级、
    无结果返回空、运行时故障向上传播（不伪装空字幕）。
    """

    @staticmethod
    def _make_engine(monkeypatch: pytest.MonkeyPatch, fake_engine: object) -> PaddleOcrEngine:
        """绕过 __init__（不下载模型），注入假 RapidOCR 实例。

        fake_engine 须为 callable：``fake_engine(image) -> 输出对象``，
        输出对象需有 ``txts`` / ``boxes`` / ``scores`` 属性（用 SimpleNamespace 构造）。
        """
        monkeypatch.setattr("sublift.ocr.paddle._PADDLE_AVAILABLE", True)
        engine = PaddleOcrEngine.__new__(PaddleOcrEngine)
        engine._engine = fake_engine  # type: ignore[assignment]
        return engine

    @staticmethod
    def _fake_engine_calling(
        txts: tuple[str, ...] | None,
        boxes: np.ndarray | None,
        scores: tuple[float, ...] | None,
    ) -> object:
        """构造一个 callable 假引擎，调用时返回带 txts/boxes/scores 的输出。"""
        output = SimpleNamespace(txts=txts, boxes=boxes, scores=scores)

        def _call(_image: object) -> object:
            return output

        return _call

    def test_box_mapping_and_order(self, monkeypatch: pytest.MonkeyPatch) -> None:
        """四角点 -> BoundingBox 包围盒；行按 (y, x) 排序。"""
        # 两个框：下方在左、上方在右，验证排序后上->下、同 y 左->右
        box_top_right = np.array([[200, 0], [300, 0], [300, 40], [200, 40]], dtype=float)
        box_bottom_left = np.array([[0, 100], [150, 100], [150, 140], [0, 140]], dtype=float)
        fake = self._fake_engine_calling(
            txts=("下", "上"),
            boxes=np.array([box_bottom_left, box_top_right]),
            scores=(0.9, 0.8),
        )
        engine = self._make_engine(monkeypatch, fake)
        result = engine.recognize(Image.new("RGB", (320, 200)))
        # 排序后「上」(y=0) 在前，「下」(y=100) 在后
        assert [ln.text for ln in result.lines] == ["上", "下"]
        assert result.lines[0].box.x == 200  # 上框 clamp 后 x
        assert result.lines[1].box.x == 0
        assert result.lines[0].confidence == 0.8
        assert result.lines[1].confidence == 0.9

    def test_boxes_none_keeps_text(self, monkeypatch: pytest.MonkeyPatch) -> None:
        """boxes 缺失但 txts 非空：用整图占位 box 保留文本。"""
        fake = self._fake_engine_calling(
            txts=("你好",),
            boxes=None,
            scores=(0.7,),
        )
        engine = self._make_engine(monkeypatch, fake)
        result = engine.recognize(Image.new("RGB", (100, 50)))
        assert result.text == "你好"
        assert len(result.lines) == 1
        assert result.lines[0].box == BoundingBox(x=0, y=0, width=100, height=50)

    def test_no_result_returns_empty(self, monkeypatch: pytest.MonkeyPatch) -> None:
        """result.txts is None（真无识别结果）返回空 OcrResult。"""
        fake = self._fake_engine_calling(txts=None, boxes=None, scores=None)
        engine = self._make_engine(monkeypatch, fake)
        result = engine.recognize(Image.new("RGB", (100, 50)))
        assert result.text == ""
        assert result.confidence == 0.0
        assert result.lines == ()

    def test_colored_pil_input_preserves_rapidocr_rgb_bgr_contract(
        self, monkeypatch: pytest.MonkeyPatch
    ) -> None:
        """彩色 PIL 输入须由 RapidOCR 转 BGR，不能先转 RGB ndarray。

        RapidOCR 对 PIL.Image 会做 RGB -> BGR 转换，而 ndarray 会被直接
        按 BGR 消费。这里用纯红像素模拟该分支：若 recognize() 回退为
        ``np.asarray(image)``，fake engine 会看到错误的 BGR 通道顺序。
        """

        class _ColorSensitiveRapidOcr:
            def __call__(self, image: Image.Image | np.ndarray) -> object:
                # 模拟 RapidOCR：PIL 来源转 RGB -> BGR，ndarray 则按 BGR 消费。
                bgr = np.asarray(image)[:, :, ::-1] if isinstance(image, Image.Image) else image

                assert tuple(bgr[0, 0]) == (0, 0, 255)
                return SimpleNamespace(
                    txts=("红色字幕",),
                    boxes=np.array([[[0, 0], [20, 0], [20, 10], [0, 10]]]),
                    scores=(0.99,),
                )

        engine = self._make_engine(monkeypatch, _ColorSensitiveRapidOcr())
        result = engine.recognize(Image.new("RGB", (40, 20), color=(255, 0, 0)))
        assert result.text == "红色字幕"
        assert result.confidence == pytest.approx(0.99)

    def test_runtime_failure_propagates(self, monkeypatch: pytest.MonkeyPatch) -> None:
        """引擎运行时故障向上传播，不伪装成空字幕。"""

        class _Boom:
            def __call__(self, _img: object) -> None:
                raise RuntimeError("ONNX 推理失败")

        engine = self._make_engine(monkeypatch, _Boom())
        with pytest.raises(RuntimeError, match="ONNX 推理失败"):
            engine.recognize(Image.new("RGB", (100, 50)))


class TestPaddleDegradation:
    """PaddleOCR 不可用时的降级测试，monkeypatch 模拟。"""

    def test_unavailable_raises(self, monkeypatch: pytest.MonkeyPatch) -> None:
        """_PADDLE_AVAILABLE 为 False 时实例化抛 RuntimeError。"""
        import sublift.ocr.paddle as paddle_mod

        monkeypatch.setattr(paddle_mod, "_PADDLE_AVAILABLE", False)
        with pytest.raises(RuntimeError, match="PaddleOCR 不可用"):
            PaddleOcrEngine()


@pytest.mark.integration
@pytest.mark.skipif(not is_paddle_available(), reason="rapidocr 未安装")
class TestPaddleOcrEngineIntegration:
    """PaddleOcrEngine 真实识别集成测试，需 rapidocr + 模型已下载。

    标 integration：构造 PaddleOcrEngine() 会触发模型下载，不应纳入默认
    pytest / init.sh（冷缓存离线环境会失败）。
    """

    def test_is_ocr_engine(self) -> None:
        """构造真实引擎，满足 OcrEngine Protocol。"""
        engine = PaddleOcrEngine()
        assert isinstance(engine, OcrEngine)

    def test_recognize_hello(self) -> None:
        """PaddleOCR 应识别出 'Hello' 英文文本。"""
        img = _make_text_image("Hello")
        engine = PaddleOcrEngine()
        result = engine.recognize(img)
        assert result.text != ""
        assert "hello" in result.text.lower()
        assert 0.0 <= result.confidence <= 1.0

    def test_recognize_chinese(self) -> None:
        """PaddleOCR 默认支持中文，应识别出中文文本。"""
        img = _make_text_image(
            "你好世界",
            width=500,
            height=120,
            font_path="/System/Library/Fonts/STHeiti Light.ttc",
            font_size=50,
        )
        engine = PaddleOcrEngine()
        result = engine.recognize(img)
        assert result.text != ""
        assert "你好世界" in result.text
        assert 0.0 <= result.confidence <= 1.0

    def test_recognize_empty_image(self) -> None:
        """纯色空白图应返回空文本。"""
        img = Image.new("RGB", (200, 100), "white")
        engine = PaddleOcrEngine()
        result = engine.recognize(img)
        assert result.text == ""
        assert result.confidence == 0.0
        assert result.lines == ()
