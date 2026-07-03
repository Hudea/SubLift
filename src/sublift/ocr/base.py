"""OCR 引擎抽象接口。"""

from __future__ import annotations

from typing import Protocol

from sublift.models import OcrResult


class OcrEngine(Protocol):
    """对图像执行 OCR 识别。"""

    def recognize(self, image: object) -> OcrResult:
        """识别图像中的文字。

        Args:
            image: 图像数据（PIL.Image）。

        Returns:
            OCR 识别结果。
        """
        ...
