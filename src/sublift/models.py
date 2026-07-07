"""SubLift 核心数据模型。

本模块定义跨模块共享的不可变数据类型。各能力模块（extractor/detector/ocr）
与串联层（pipeline/export）均依赖这些类型，避免循环依赖。
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Any

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
class OcrLine:
    """OCR 单行识别结果（feat-033a）。

    保留 Vision 每行 observation 的文本、置信度与 bbox，
    供字幕行 selector 按 y 轨道/行高筛选目标字幕层。

    bbox 为裁剪图内绝对像素坐标（左上原点），与 OcrEngine 收到的裁剪图同坐标系。
    """

    text: str
    confidence: float
    bbox: BoundingBox


@dataclass(frozen=True)
class OcrResult:
    """OCR 识别结果。

    text/confidence 为扁平字段（向后兼容）；lines 保留 per-line observation
    （feat-033a）。无 profile 时走 text/confidence 旧路径，有 profile 时用 lines
    跑 selector。
    """

    text: str
    confidence: float
    lines: list[OcrLine] = field(default_factory=list)


@dataclass(frozen=True)
class SubtitleEntry:
    """字幕条目，含起止时间、文本与置信度。"""

    start_ms: int
    end_ms: int
    text: str
    confidence: float = 1.0


@dataclass(frozen=True)
class SubtitleProfile:
    """目标字幕层约束（feat-033b）。

    描述 ROI 内「相信哪一层文字」，与 region_box（「看哪里」）正交。
    selector 据此从 OCR observations 中筛选目标字幕行，过滤背景英文/水印/标牌。

    所有 y 坐标均为裁剪图内绝对像素（左上原点），与 OcrLine.bbox 同坐标系。
    GUI 生成时需把全帧候选框坐标减去 region_box 的 x/y 偏移。
    """

    y_center: float
    y_tolerance: float
    line_height: float
    max_lines: int = 1
    script_hint: str = "auto"

    @classmethod
    def from_dict(cls, d: dict[str, Any]) -> SubtitleProfile:
        """从 IPC subtitle_profile 字典构造（feat-033b）。"""
        return cls(
            y_center=float(d["y_center"]),
            y_tolerance=float(d["y_tolerance"]),
            line_height=float(d["line_height"]),
            max_lines=int(d.get("max_lines", 1)),
            script_hint=str(d.get("script_hint", "auto")),
        )
