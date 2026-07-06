"""SubLift 核心数据模型。

本模块定义跨模块共享的不可变数据类型。各能力模块（extractor/detector/ocr）
与串联层（pipeline/export）均依赖这些类型，避免循环依赖。
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from PIL import Image


@dataclass(frozen=True)
class BoundingBox:
    """矩形区域，绝对像素坐标。"""

    x: int
    y: int
    width: int
    height: int


@dataclass(frozen=True)
class Region:
    """字幕区域，由 BoundingBox 描述。"""

    box: BoundingBox


@dataclass(frozen=True)
class Frame:
    """视频帧，附带时间戳。"""

    timestamp_ms: int
    image: Image.Image


@dataclass(frozen=True)
class OcrResult:
    """OCR 识别结果。"""

    text: str
    confidence: float


@dataclass(frozen=True)
class SubtitleEntry:
    """字幕条目，含起止时间、文本与置信度。"""

    start_ms: int
    end_ms: int
    text: str
    confidence: float = 1.0
