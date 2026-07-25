"""PaddleOCR 引擎，通过 rapidocr 桥接 PP-OCRv6。

跨平台 OCR 第二引擎（feat-05001）。导入失败时优雅降级：
模块级 _PADDLE_AVAILABLE 标志不阻断模块加载，实例化 PaddleOcrEngine 时抛
RuntimeError 提示安装可选依赖。

默认模型规格为 PP-OCRv6 small，暴露 model_type 参数可切换 tiny/medium。
模型缓存覆盖为 ~/.cache/sublift/rapidocr-models，跨 venv 复用。

行级输出：每个检测框映射为 OcrLine(text, confidence, box)；
OcrResult.text/confidence 由 from_lines 兼容 join（\\n + 均值 conf）。

不接 Phase 4.2 归因系统（timing_callback），契约允许。
"""

from __future__ import annotations

import logging
from pathlib import Path
from typing import TYPE_CHECKING

import numpy as np

from sublift.models import BoundingBox, OcrLine, OcrResult

if TYPE_CHECKING:
    from PIL import Image
    from rapidocr.utils.typings import ModelType

try:
    from rapidocr import RapidOCR
    from rapidocr.utils.typings import ModelType

    _PADDLE_AVAILABLE: bool = True
    _IMPORT_ERROR: str | None = None
except ImportError as e:
    _PADDLE_AVAILABLE = False
    _IMPORT_ERROR = str(e)

logger = logging.getLogger(__name__)

DEFAULT_MODEL_DIR: Path = Path.home() / ".cache" / "sublift" / "rapidocr-models"

_VALID_MODEL_TYPES = frozenset({"tiny", "small", "medium"})


def _str_to_model_type(name: str) -> ModelType:
    """将字符串模型规格转为 rapidocr ModelType 枚举。"""
    key = name.lower()
    if key not in _VALID_MODEL_TYPES:
        raise ValueError(
            f"未知 model_type: {name!r}，可选: {sorted(_VALID_MODEL_TYPES)}"
        )
    return ModelType[key.upper()]


def is_paddle_available() -> bool:
    """返回 PaddleOCR（rapidocr）是否可用（是否已安装）。"""
    return _PADDLE_AVAILABLE


class PaddleOcrEngine:
    """通过 rapidocr（PP-OCRv6）识别图像中文字的 OCR 引擎。

    rapidocr 未安装时实例化抛 RuntimeError。
    不接 Phase 4.2 归因系统（timing_callback），契约允许。
    """

    def __init__(
        self,
        model_type: str = "small",
        model_root_dir: Path | str | None = None,
    ) -> None:
        """初始化 PaddleOCR 引擎。

        Args:
            model_type: 模型规格（``tiny`` / ``small`` / ``medium``），
                默认 ``small``。影响 Det 和 Rec 两个阶段。
            model_root_dir: 模型缓存目录，默认
                ``~/.cache/sublift/rapidocr-models``。
                首次构造时 rapidocr 自动下载模型到该目录。

        Raises:
            RuntimeError: rapidocr 未安装，PaddleOCR 不可用。
        """
        if not _PADDLE_AVAILABLE:
            raise RuntimeError(
                "PaddleOCR 不可用。请安装可选依赖："
                "uv sync --extra paddle"
                + (f"（导入错误: {_IMPORT_ERROR}）" if _IMPORT_ERROR else "")
            )
        root = Path(model_root_dir) if model_root_dir else DEFAULT_MODEL_DIR
        model_enum = _str_to_model_type(model_type)
        self._engine = RapidOCR(
            params={
                "Global.model_root_dir": str(root),
                "Det.model_type": model_enum,
                "Rec.model_type": model_enum,
            }
        )

    def recognize(self, image: Image.Image) -> OcrResult:
        """用 PaddleOCR 识别图像中的文字。

        异常兜底返回空 OcrResult，不崩溃。

        Args:
            image: PIL.Image 图像。

        Returns:
            带 ``lines`` 的 OCR 结果；``text``/``confidence`` 为兼容汇总。
            无识别结果或异常时返回 OcrResult("", 0.0)。
        """
        try:
            arr = np.array(image.convert("RGB"))
            result = self._engine(arr)

            # rapidocr 返回类型是联合类型；当传入 ndarray 时为 RapidOCROutput
            if result.txts is None:  # type: ignore[union-attr]
                return OcrResult.from_lines([])

            lines: list[OcrLine] = []
            boxes = result.boxes  # type: ignore[union-attr]
            txts: tuple[str, ...] = result.txts  # type: ignore[union-attr]
            scores_raw = result.scores or ()  # type: ignore[union-attr]

            if boxes is None:
                return OcrResult.from_lines([])

            for i, (box_corners, text) in enumerate(zip(boxes, txts, strict=True)):
                if not text.strip():
                    continue
                # 四角点 → axis-aligned 包围盒
                xs = box_corners[:, 0]
                ys = box_corners[:, 1]
                x_min = int(np.floor(xs.min()))
                y_min = int(np.floor(ys.min()))
                x_max = int(np.ceil(xs.max()))
                y_max = int(np.ceil(ys.max()))
                # clamp 到图像范围
                img_h, img_w = arr.shape[:2]
                x_min = max(0, min(x_min, img_w))
                y_min = max(0, min(y_min, img_h))
                x_max = max(0, min(x_max, img_w))
                y_max = max(0, min(y_max, img_h))
                bbox = BoundingBox(
                    x=x_min,
                    y=y_min,
                    width=x_max - x_min,
                    height=y_max - y_min,
                )
                conf = scores_raw[i] if i < len(scores_raw) else 0.0
                lines.append(OcrLine(text=text, confidence=conf, box=bbox))

            # 稳定行序：上→下，同 y 左→右
            lines.sort(key=lambda line: (line.box.y, line.box.x))
            return OcrResult.from_lines(lines)
        except Exception:
            logger.debug("PaddleOCR recognize 异常，返回空结果", exc_info=True)
            return OcrResult.from_lines([])
