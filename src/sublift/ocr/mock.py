"""Mock OCR 引擎，测试用。

支持两种返回模式：
- 固定模式：每次 recognize 返回相同的 OcrResult。
- 序列模式：按调用顺序依次返回 sequence 中的结果，用于 pipeline 闭环测试
  模拟字幕逐帧变化。

固定模式可选 ``lines``：非 None 时用 :meth:`OcrResult.from_lines` 生成结果。
"""

from __future__ import annotations

from collections.abc import Sequence
from typing import TYPE_CHECKING

from sublift.models import OcrLine, OcrResult

if TYPE_CHECKING:
    from PIL import Image


class MockOcrEngine:
    """测试用 OCR 引擎，返回预设的固定或序列结果。"""

    def __init__(
        self,
        text: str = "",
        confidence: float = 1.0,
        sequence: list[OcrResult] | None = None,
        lines: Sequence[OcrLine] | None = None,
    ) -> None:
        """初始化 Mock OCR 引擎。

        Args:
            text: 固定模式返回的文本（sequence 为 None 且 lines 为 None 时生效）。
            confidence: 固定模式返回的置信度（同上）。
            sequence: 序列模式结果列表，非 None 时按调用顺序返回；
                越界调用抛 IndexError（暴露测试 bug）。
            lines: 固定模式行级结果；非 None 时忽略 text/confidence，
                使用 :meth:`OcrResult.from_lines`。
        """
        if lines is not None:
            self._fixed_result = OcrResult.from_lines(lines)
        else:
            self._fixed_result = OcrResult(text=text, confidence=confidence)
        self._sequence = sequence
        self._index = 0

    def recognize(self, image: Image.Image) -> OcrResult:
        """返回预设的 OCR 结果。

        Args:
            image: 图像数据（Mock 不使用，仅满足 Protocol 签名）。

        Returns:
            固定模式返回预设 OcrResult；序列模式按顺序返回，越界抛 IndexError。
        """
        if self._sequence is not None:
            result = self._sequence[self._index]
            self._index += 1
            return result
        return self._fixed_result
