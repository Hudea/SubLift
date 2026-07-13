"""OCR 引擎能力模块。"""

from sublift.ocr.base import OcrEngine
from sublift.ocr.mock import MockOcrEngine
from sublift.ocr.vision import VisionOcrEngine, is_vision_available

__all__ = ["MockOcrEngine", "OcrEngine", "VisionOcrEngine", "is_vision_available"]
