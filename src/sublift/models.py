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
class PersistentTextPolicy:
    """持久背景文字过滤策略（feat-034）。

    描述如何通过跨段时序统计识别并剔除持久背景文字（ticker / 水印）。
    双条件算法（任一命中即判为持久背景）：

    - 条件 A（固定水印）：同一文本指纹在同一 y_bin 连续出现段数
      ≥ min_repeat_segments。
    - 条件 B（ticker 文本片段多变）：同一 y_bin 累计不同文本指纹数
      ≥ min_distinct_texts，整个 y_bin 判为持久背景带。

    Attributes:
        enabled: 是否启用持久背景过滤。False 时 filter 跳过。
        min_repeat_segments: 条件 A 阈值（同文本连续重复段数）。
        min_distinct_texts: 条件 B 阈值（同 y_bin 不同文本数）。
        y_bin_ratio: y_bin 划分粒度，bin = line_height * 此值。
            同 bin 视为同 y 轨道。
    """

    enabled: bool = True
    min_repeat_segments: int = 3
    min_distinct_texts: int = 4
    y_bin_ratio: float = 0.5

    @classmethod
    def from_dict(cls, d: dict[str, Any]) -> PersistentTextPolicy:
        """从 IPC persistent_text_policy 字典构造（feat-034a）。"""
        return cls(
            enabled=bool(d.get("enabled", True)),
            min_repeat_segments=int(d.get("min_repeat_segments", 3)),
            min_distinct_texts=int(d.get("min_distinct_texts", 4)),
            y_bin_ratio=float(d.get("y_bin_ratio", 0.5)),
        )


@dataclass(frozen=True)
class SubtitleProfile:
    """目标字幕层约束（feat-033b）。

    描述 ROI 内「相信哪一层文字」，与 region_box（「看哪里」）正交。
    selector 据此从 OCR observations 中筛选目标字幕行，过滤背景英文/水印/标牌。

    所有 y 坐标均为裁剪图内绝对像素（左上原点），与 OcrLine.bbox 同坐标系。
    GUI 生成时需把全帧候选框坐标减去 region_box 的 x/y 偏移。

    persistent_text_policy（feat-034）描述跨段时序过滤策略，用于识别
    同带同高、水平重叠的 ticker / 水印。None 时不做跨段过滤。
    """

    y_center: float
    y_tolerance: float
    line_height: float
    max_lines: int = 1
    script_hint: str = "auto"
    persistent_text_policy: PersistentTextPolicy | None = None

    @classmethod
    def from_dict(cls, d: dict[str, Any]) -> SubtitleProfile:
        """从 IPC subtitle_profile 字典构造（feat-033b / feat-034a）。"""
        policy_dict = d.get("persistent_text_policy")
        policy = (
            PersistentTextPolicy.from_dict(policy_dict)
            if isinstance(policy_dict, dict)
            else None
        )
        return cls(
            y_center=float(d["y_center"]),
            y_tolerance=float(d["y_tolerance"]),
            line_height=float(d["line_height"]),
            max_lines=int(d.get("max_lines", 1)),
            script_hint=str(d.get("script_hint", "auto")),
            persistent_text_policy=policy,
        )
