"""Apple Vision OCR 引擎，通过 PyObjC 桥接 VNRecognizeTextRequest。

平台特定 API 唯一容身处（ADR-0002）。导入失败时优雅降级：
模块级 _VISION_AVAILABLE 标志不阻断模块加载，实例化 VisionOcrEngine 时抛
RuntimeError 提示安装可选依赖。

默认识别语言为简体中文+英文（zh-Hans, en-US），覆盖 SubLift 核心场景。
VNRecognizeTextRequest 的默认 recognitionLanguages 仅 en-US，无法识别中文，
故必须显式设置。
"""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

from sublift.models import OcrResult

if TYPE_CHECKING:
    from PIL import Image

try:
    import Vision
    from Quartz import (
        CGColorSpaceCreateDeviceRGB,
        CGDataProviderCreateWithData,
        CGImageCreate,
    )

    _VISION_AVAILABLE = True
    _IMPORT_ERROR: str | None = None
except ImportError as e:
    _VISION_AVAILABLE = False
    _IMPORT_ERROR = str(e)

DEFAULT_RECOGNITION_LANGUAGES: list[str] = ["zh-Hans", "en-US"]


def is_vision_available() -> bool:
    """返回 Apple Vision 是否可用（PyObjC 是否已安装）。"""
    return _VISION_AVAILABLE


class VisionOcrEngine:
    """通过 Apple Vision 识别图像中文字的 OCR 引擎。

    PyObjC 未安装时实例化抛 RuntimeError。
    """

    def __init__(
        self,
        recognition_languages: list[str] | None = None,
    ) -> None:
        """初始化 Vision OCR 引擎。

        Args:
            recognition_languages: 识别语言列表（BCP-47），默认
                ["zh-Hans", "en-US"]（简体中文+英文）。传 None 用默认值。
                VNRecognizeTextRequest 默认仅 en-US，无法识别中文，故需显式设置。

        Raises:
            RuntimeError: PyObjC 未安装，Vision 不可用。
        """
        if not _VISION_AVAILABLE:
            raise RuntimeError(
                "Apple Vision 不可用。请安装 macOS 可选依赖："
                "uv sync --extra vision"
                + (f"（导入错误: {_IMPORT_ERROR}）" if _IMPORT_ERROR else "")
            )
        self._recognition_languages = (
            recognition_languages
            if recognition_languages is not None
            else DEFAULT_RECOGNITION_LANGUAGES
        )

    def recognize(self, image: Image.Image) -> OcrResult:
        """用 Apple Vision 识别图像中的文字。

        Args:
            image: PIL.Image 图像。

        Returns:
            OCR 识别结果。多行文本以 "\\n" 连接，置信度取各识别结果均值；
            无识别结果返回 OcrResult("", 0.0)。
        """
        cgimage = _pil_to_cgimage(image)
        handler = Vision.VNImageRequestHandler.alloc().initWithCGImage_options_(
            cgimage, None
        )
        request = Vision.VNRecognizeTextRequest.alloc().init()
        request.setRecognitionLanguages_(self._recognition_languages)

        success, _error = handler.performRequests_error_([request], None)
        if not success:
            return OcrResult(text="", confidence=0.0)

        return _collect_results(request)


def _pil_to_cgimage(image: Image.Image) -> Any:
    """将 PIL.Image 转换为 CGImage。

    Args:
        image: PIL.Image 图像。

    Returns:
        Quartz CGImage 对象。
    """
    rgb_image = image.convert("RGB")
    width, height = rgb_image.size
    raw_bytes = rgb_image.tobytes()

    provider = CGDataProviderCreateWithData(None, raw_bytes, len(raw_bytes), None)
    colorspace = CGColorSpaceCreateDeviceRGB()
    return CGImageCreate(
        width,
        height,
        8,
        24,
        width * 3,
        colorspace,
        0,
        provider,
        None,
        False,
        0,
    )


def _collect_results(request: Any) -> OcrResult:
    """从 VNRecognizeTextRequest 收集识别结果。

    Args:
        request: 已执行的 VNRecognizeTextRequest。

    Returns:
        合并后的 OcrResult。多行文本以 "\\n" 连接，置信度取均值；
        无结果返回 OcrResult("", 0.0)。
    """
    observations: list[Any] = request.results()
    if not observations:
        return OcrResult(text="", confidence=0.0)

    texts: list[str] = []
    confidences: list[float] = []
    for obs in observations:
        candidates = obs.topCandidates_(1)
        if candidates:
            candidate = candidates[0]
            texts.append(candidate.string())
            confidences.append(float(candidate.confidence()))

    if not texts:
        return OcrResult(text="", confidence=0.0)

    avg_conf = sum(confidences) / len(confidences)
    return OcrResult(text="\n".join(texts), confidence=avg_conf)
