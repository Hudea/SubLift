"""Apple Vision OCR 引擎，通过 PyObjC 桥接 VNRecognizeTextRequest。

平台特定 API 唯一容身处（ADR-0002）。导入失败时优雅降级：
模块级 _VISION_AVAILABLE 标志不阻断模块加载，实例化 VisionOcrEngine 时抛
RuntimeError 提示安装可选依赖。

默认识别语言为简体中文+英文（zh-Hans, en-US），覆盖 SubLift 核心场景。
VNRecognizeTextRequest 的默认 recognitionLanguages 仅 en-US，无法识别中文，
故必须显式设置。

行级输出：每个 observation 映射为 OcrLine(text, confidence, box)；
OcrResult.text/confidence 由 from_lines 兼容 join（\\n + 均值 conf）。
"""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

from sublift.models import BoundingBox, OcrLine, OcrResult

if TYPE_CHECKING:
    from PIL import Image

try:
    import Vision
    from Quartz import (
        CGColorSpaceCreateDeviceRGB,
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
            带 ``lines`` 的 OCR 结果；``text``/``confidence`` 为兼容汇总。
            无识别结果返回 OcrResult("", 0.0)。
        """
        import objc  # type: ignore
        with objc.autorelease_pool():
            width, height = image.size
            cgimage = _pil_to_cgimage(image)
            handler = Vision.VNImageRequestHandler.alloc().initWithCGImage_options_(
                cgimage, None
            )
            request = Vision.VNRecognizeTextRequest.alloc().init()
            request.setRecognitionLanguages_(self._recognition_languages)

            success, _error = handler.performRequests_error_([request], None)
            if not success:
                return OcrResult(text="", confidence=0.0)

            return _collect_results(request, (width, height))


def _pil_to_cgimage(image: Image.Image) -> Any:
    """将 PIL.Image 转换为 CGImage。

    Args:
        image: PIL.Image 图像。

    Returns:
        Quartz CGImage 对象。
    """
    rgb_image = image.convert("RGB")
    width, height = rgb_image.size
    from Foundation import NSData
    from Quartz import CGDataProviderCreateWithCFData

    raw_bytes = rgb_image.tobytes()
    ns_data = NSData.dataWithBytes_length_(raw_bytes, len(raw_bytes))
    provider = CGDataProviderCreateWithCFData(ns_data)

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


def _vision_box_to_pixel(
    bbox: Any,
    width: int,
    height: int,
) -> BoundingBox:
    """将 Vision 归一化 boundingBox（原点左下）转为像素 BoundingBox（原点左上）。

    支持带 origin/size 属性的对象，或 ``((x, y), (w, h))`` / 扁平四元组。
    """
    nx, ny, nw, nh = _unpack_normalized_rect(bbox)
    # Vision: origin 左下；像素: 原点左上
    x = round(nx * width)
    y = round((1.0 - ny - nh) * height)
    w = round(nw * width)
    h = round(nh * height)
    return _clamp_box(x, y, w, h, width, height)


def _unpack_normalized_rect(bbox: Any) -> tuple[float, float, float, float]:
    """从多种 PyObjC / 测试假对象形态解包 (x, y, w, h)。"""
    if hasattr(bbox, "origin") and hasattr(bbox, "size"):
        origin = bbox.origin
        size = bbox.size
        if hasattr(origin, "x"):
            return (
                float(origin.x),
                float(origin.y),
                float(size.width),
                float(size.height),
            )
        # origin/size 可能是 (x, y) / (w, h) 元组
        return (
            float(origin[0]),
            float(origin[1]),
            float(size[0]),
            float(size[1]),
        )
    if isinstance(bbox, (list, tuple)):
        if len(bbox) == 2 and isinstance(bbox[0], (list, tuple)):
            return (
                float(bbox[0][0]),
                float(bbox[0][1]),
                float(bbox[1][0]),
                float(bbox[1][1]),
            )
        if len(bbox) == 4:
            return (float(bbox[0]), float(bbox[1]), float(bbox[2]), float(bbox[3]))
    # 命名属性 x/y/width/height
    if all(hasattr(bbox, name) for name in ("x", "y", "width", "height")):
        return (
            float(bbox.x),
            float(bbox.y),
            float(bbox.width),
            float(bbox.height),
        )
    raise TypeError(f"unsupported Vision boundingBox type: {type(bbox)!r}")


def _clamp_box(
    x: int,
    y: int,
    w: int,
    h: int,
    width: int,
    height: int,
) -> BoundingBox:
    """将 box clamp 到图像范围内。"""
    if width <= 0 or height <= 0:
        return BoundingBox(x=0, y=0, width=0, height=0)
    x = max(0, min(x, width))
    y = max(0, min(y, height))
    w = max(0, min(w, width - x))
    h = max(0, min(h, height - y))
    return BoundingBox(x=x, y=y, width=w, height=h)


def _collect_results(
    request: Any,
    image_size: tuple[int, int],
) -> OcrResult:
    """从 VNRecognizeTextRequest 收集行级识别结果。

    Args:
        request: 已执行的 VNRecognizeTextRequest。
        image_size: ``(width, height)`` 像素，用于 box 换算。

    Returns:
        带 lines 的 OcrResult；兼容 text/confidence 由 from_lines 生成。
        无结果返回 OcrResult("", 0.0)。
    """
    observations: list[Any] = request.results() or []
    width, height = image_size
    lines: list[OcrLine] = []

    for obs in observations:
        candidates = obs.topCandidates_(1)
        if not candidates:
            continue
        candidate = candidates[0]
        text = str(candidate.string())
        if not text.strip():
            continue
        conf = float(candidate.confidence())
        try:
            box = _vision_box_to_pixel(obs.boundingBox(), width, height)
        except (TypeError, AttributeError, IndexError, ValueError):
            # box 解析失败时仍保留文本，用整图占位便于下游降级
            box = BoundingBox(x=0, y=0, width=max(0, width), height=max(0, height))
        lines.append(OcrLine(text=text, confidence=conf, box=box))

    # 稳定行序：上→下，同 y 左→右
    lines.sort(key=lambda line: (line.box.y, line.box.x))
    return OcrResult.from_lines(lines)
