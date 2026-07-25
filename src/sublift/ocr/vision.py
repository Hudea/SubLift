"""Apple Vision OCR 引擎，通过 PyObjC 桥接 VNRecognizeTextRequest。

平台特定 API 唯一容身处（ADR-0002）。导入失败时优雅降级：
模块级 _VISION_AVAILABLE 标志不阻断模块加载，实例化 VisionOcrEngine 时抛
RuntimeError 提示安装可选依赖。

默认识别语言为简体中文+英文（zh-Hans, en-US），覆盖 SubLift 核心场景。
VNRecognizeTextRequest 的默认 recognitionLanguages 仅 en-US，无法识别中文，
故必须显式设置。

行级输出：每个 observation 映射为 OcrLine(text, confidence, box)；
OcrResult.text/confidence 由 from_lines 兼容 join（\\n + 均值 conf）。

feat-043b：可选 timing_callback 在每次 recognize 调用后产出 :class:`OcrCallDetail`，
包括 input_prepare / request_setup / vision_perform / observation_mapping / residual
五个内部阶段；OcrEngine Protocol 不改变。
"""

from __future__ import annotations

import time
from collections.abc import Callable
from contextlib import suppress
from typing import TYPE_CHECKING, Any

from sublift.models import BoundingBox, OcrLine, OcrResult

if TYPE_CHECKING:
    from PIL import Image

    from sublift.diagnostics.performance import OcrCallDetail

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


def _import_objc() -> Any:
    """延迟导入 PyObjC 运行时（仅 recognize 路径需要）。"""
    import objc

    return objc


class VisionOcrEngine:
    """通过 Apple Vision 识别图像中文字的 OCR 引擎。

    PyObjC 未安装时实例化抛 RuntimeError。

    feat-043b：可选 ``timing_callback`` 在每次 ``recognize()`` 完成后被调用，
    传递包含五阶段内部计时的 :class:`~sublift.diagnostics.performance.OcrCallDetail`。
    空结果/失败也会触发回调；不提供回调时零开销（仅一次 None 判断）。
    """

    def __init__(
        self,
        recognition_languages: list[str] | None = None,
        *,
        timing_callback: Callable[[OcrCallDetail], None] | None = None,
    ) -> None:
        """初始化 Vision OCR 引擎。

        Args:
            recognition_languages: 识别语言列表（BCP-47），默认
                ["zh-Hans", "en-US"]（简体中文+英文）。传 None 用默认值。
                VNRecognizeTextRequest 默认仅 en-US，无法识别中文，故需显式设置。
            timing_callback: 可选，每次 recognize() 调用后接收
                :class:`~sublift.diagnostics.performance.OcrCallDetail`
                的内部计时明细回调。None 时不记录（零开销）。

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
        self._timing_callback = timing_callback

    def recognize(self, image: Image.Image) -> OcrResult:
        """用 Apple Vision 识别图像中的文字。

        feat-043b：当 ``_timing_callback`` 非 None 时，在每个内部阶段计时并
        在返回前触发回调；空结果/失败也产生完整计时记录。无 callback 时走
        快速路径，不做 ``perf_counter`` 分阶段计时（仅一次 None 判断）。

        Args:
            image: PIL.Image 图像。

        Returns:
            带 ``lines`` 的 OCR 结果；``text``/``confidence`` 为兼容汇总。
            无识别结果返回 OcrResult("", 0.0)。
        """
        if self._timing_callback is None:
            return self._recognize_untimed(image)
        return self._recognize_timed(image)

    def _recognize_untimed(self, image: Image.Image) -> OcrResult:
        """默认产品路径：无 observer，不做分阶段计时。"""
        objc = _import_objc()
        width, height = image.size
        with objc.autorelease_pool():
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

    def _recognize_timed(self, image: Image.Image) -> OcrResult:
        """带内部归因的路径：五阶段计时 + 可选 callback（异常隔离）。"""
        objc = _import_objc()
        t_total_start = time.perf_counter()

        # 延迟求值：仅在异常发生在 image.size 之前时使用占位值
        width: int = 0
        height: int = 0
        input_mode: str = "unknown"
        try:
            width, height = image.size
            input_mode = image.mode
        except Exception:
            t_total = max(0.0, time.perf_counter() - t_total_start)
            self._fire_timing(
                width=width,
                height=height,
                input_mode=input_mode,
                t_input_prepare=0.0,
                t_request_setup=0.0,
                t_vision_perform=0.0,
                t_observation_mapping=0.0,
                t_residual=t_total,
                t_total=t_total,
                outcome="error",
            )
            raise

        try:
            with objc.autorelease_pool():
                # 1) input_prepare: PIL → CGImage
                t0 = time.perf_counter()
                cgimage = _pil_to_cgimage(image)
                t_input_prepare = max(0.0, time.perf_counter() - t0)

                # 2) request_setup: handler + request + language config
                t0 = time.perf_counter()
                handler = Vision.VNImageRequestHandler.alloc().initWithCGImage_options_(
                    cgimage, None
                )
                request = Vision.VNRecognizeTextRequest.alloc().init()
                request.setRecognitionLanguages_(self._recognition_languages)
                t_request_setup = max(0.0, time.perf_counter() - t0)

                # 3) vision_perform: the actual Vision inference
                t0 = time.perf_counter()
                success, _error = handler.performRequests_error_([request], None)
                t_vision_perform = max(0.0, time.perf_counter() - t0)

                # 4) observation_mapping: observations → OcrLine / OcrResult
                t0 = time.perf_counter()
                if not success:
                    result = OcrResult(text="", confidence=0.0)
                else:
                    result = _collect_results(request, (width, height))
                t_observation_mapping = max(0.0, time.perf_counter() - t0)

            # 5) residual: autorelease pool exit + Python call overhead
            t_total = max(0.0, time.perf_counter() - t_total_start)
            components = (
                t_input_prepare
                + t_request_setup
                + t_vision_perform
                + t_observation_mapping
            )
            t_residual = max(0.0, t_total - components)

            if not success:
                outcome = "error"
            elif not result.text.strip():
                outcome = "empty"
            else:
                outcome = "success"

            self._fire_timing(
                width=width,
                height=height,
                input_mode=input_mode,
                t_input_prepare=t_input_prepare,
                t_request_setup=t_request_setup,
                t_vision_perform=t_vision_perform,
                t_observation_mapping=t_observation_mapping,
                t_residual=t_residual,
                t_total=t_total,
                outcome=outcome,
            )
            return result
        except Exception:
            # 异常捕获：仍产出完整 error 记录供对账
            t_total = max(0.0, time.perf_counter() - t_total_start)
            self._fire_timing(
                width=width,
                height=height,
                input_mode=input_mode,
                t_input_prepare=0.0,
                t_request_setup=0.0,
                t_vision_perform=0.0,
                t_observation_mapping=0.0,
                t_residual=t_total,
                t_total=t_total,
                outcome="error",
            )
            raise

    def _fire_timing(
        self,
        *,
        width: int,
        height: int,
        input_mode: str,
        t_input_prepare: float,
        t_request_setup: float,
        t_vision_perform: float,
        t_observation_mapping: float,
        t_residual: float,
        t_total: float,
        outcome: str,
    ) -> None:
        """构造 OcrCallDetail 并触发 timing_callback。

        callback 异常被吞掉，不得改变 OCR 返回值或原有异常语义。
        """
        from sublift.diagnostics.performance import OcrCallDetail

        detail = OcrCallDetail(
            input_width=width,
            input_height=height,
            input_mode=input_mode,
            input_prepare_ms=t_input_prepare * 1000.0,
            request_setup_ms=t_request_setup * 1000.0,
            vision_perform_ms=t_vision_perform * 1000.0,
            observation_mapping_ms=t_observation_mapping * 1000.0,
            residual_ms=t_residual * 1000.0,
            total_ms=t_total * 1000.0,
            outcome=outcome,
        )
        callback = self._timing_callback
        if callback is None:
            return
        # observer 是诊断旁路；失败不得影响产品 OCR 语义
        with suppress(Exception):
            callback(detail)


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
