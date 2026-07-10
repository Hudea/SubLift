"""SubLift 核心数据模型。

本模块定义跨模块共享的不可变数据类型。各能力模块（extractor/detector/ocr）
与串联层（pipeline/export）均依赖这些类型，避免循环依赖。
"""

from __future__ import annotations

from collections.abc import Mapping, Sequence
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
class OcrLine:
    """单行 OCR 结果。

    ``box`` 相对本次 ``recognize`` 的输入图像，像素坐标，原点左上。
    """

    text: str
    confidence: float
    box: BoundingBox


# 合法文字系统取值（feat-034b SubtitleProfile.script）
SCRIPT_CJK = "cjk"
SCRIPT_LATIN = "latin"
SCRIPT_AUTO = "auto"
SCRIPT_VALUES = frozenset({SCRIPT_CJK, SCRIPT_LATIN, SCRIPT_AUTO})


@dataclass(frozen=True)
class SubtitleProfile:
    """字幕轨画像，供行级选择（feat-034）。

    几何字段均在 **OCR crop 坐标系**（与 :class:`OcrLine.box` 相同）：
    原点为 ``region_box`` 裁剪图左上角，单位像素。

    Attributes:
        script: 目标文字系统（``cjk`` / ``latin`` / ``auto``）。
        center_x: 期望字幕中心 X。
        center_y: 期望字幕中心 Y。
        height: 期望字号/行高（像素）。
        y_min: 期望垂直带上沿。
        y_max: 期望垂直带下沿。
    """

    script: str = SCRIPT_CJK
    center_x: int = 0
    center_y: int = 0
    height: int = 0
    y_min: int = 0
    y_max: int = 0

    @staticmethod
    def from_crop(
        width: int,
        height: int,
        *,
        script: str = SCRIPT_CJK,
    ) -> SubtitleProfile:
        """由 crop 尺寸推导默认 profile（CLI/benchmark：整带居中）。"""
        w = max(0, width)
        h = max(0, height)
        return SubtitleProfile(
            script=script,
            center_x=w // 2,
            center_y=h // 2,
            height=h,
            y_min=0,
            y_max=h,
        )

    @staticmethod
    def from_selection_in_video(
        region: BoundingBox,
        selection: BoundingBox,
        *,
        script: str = SCRIPT_CJK,
    ) -> SubtitleProfile:
        """将视频像素选区映射为相对 ``region`` crop 的 profile。

        Args:
            region: 最终 OCR 裁剪区（视频像素，通常 X 全宽）。
            selection: 用户选中的字幕带（视频像素，可为窄框并集）。
            script: 目标文字系统。
        """
        # selection 相对 crop 的包围盒
        rel_x = selection.x - region.x
        rel_y = selection.y - region.y
        # clamp 到 crop
        x0 = max(0, min(rel_x, max(0, region.width)))
        y0 = max(0, min(rel_y, max(0, region.height)))
        x1 = max(x0, min(rel_x + selection.width, max(0, region.width)))
        y1 = max(y0, min(rel_y + selection.height, max(0, region.height)))
        band_w = max(0, x1 - x0)
        band_h = max(0, y1 - y0)
        return SubtitleProfile(
            script=script,
            center_x=x0 + band_w // 2,
            center_y=y0 + band_h // 2,
            height=band_h if band_h > 0 else max(0, region.height),
            y_min=y0,
            y_max=y1 if y1 > y0 else max(0, region.height),
        )

    def to_dict(self) -> dict[str, str | int]:
        """序列化为 IPC / JSON 字典。"""
        return {
            "script": self.script,
            "center_x": self.center_x,
            "center_y": self.center_y,
            "height": self.height,
            "y_min": self.y_min,
            "y_max": self.y_max,
        }

    @staticmethod
    def from_dict(data: Mapping[str, object]) -> SubtitleProfile:
        """从 IPC / JSON 字典解析。

        Raises:
            ValueError: 字段缺失、类型错误或 script 非法。
        """
        script_raw = data.get("script", SCRIPT_CJK)
        if not isinstance(script_raw, str) or script_raw not in SCRIPT_VALUES:
            raise ValueError(f"subtitle_profile.script 非法: {script_raw!r}")

        def _int_field(key: str) -> int:
            if key not in data:
                raise ValueError(f"subtitle_profile.{key} 缺失")
            val = data[key]
            if isinstance(val, bool) or not isinstance(val, int):
                raise ValueError(f"subtitle_profile.{key} 非整数: {val!r}")
            return val

        return SubtitleProfile(
            script=script_raw,
            center_x=_int_field("center_x"),
            center_y=_int_field("center_y"),
            height=_int_field("height"),
            y_min=_int_field("y_min"),
            y_max=_int_field("y_max"),
        )


@dataclass(frozen=True)
class OcrResult:
    """OCR 识别结果。

    ``text`` / ``confidence`` 为兼容汇总（多行 ``\\n`` 连接 + 置信度均值）；
    行级语义以 ``lines`` 为准。引擎应优先填充 ``lines``，再用
    :meth:`from_lines` 生成汇总字段。
    """

    text: str
    confidence: float
    lines: tuple[OcrLine, ...] = ()

    @staticmethod
    def from_lines(lines: Sequence[OcrLine]) -> OcrResult:
        """由行级结果构造 OcrResult（兼容 join）。

        空序列返回 ``OcrResult("", 0.0, lines=())``。
        非空时 ``text`` 为各行 text 以 ``\\n`` 连接，``confidence`` 为均值。
        """
        if not lines:
            return OcrResult(text="", confidence=0.0, lines=())
        line_tuple = tuple(lines)
        text = "\n".join(line.text for line in line_tuple)
        confidence = sum(line.confidence for line in line_tuple) / len(line_tuple)
        return OcrResult(text=text, confidence=confidence, lines=line_tuple)


@dataclass(frozen=True)
class SubtitleEntry:
    """字幕条目，含起止时间、文本与置信度。"""

    start_ms: int
    end_ms: int
    text: str
    confidence: float = 1.0
