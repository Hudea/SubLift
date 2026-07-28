"""帧 IO 规划：source-frame region → extractor crop + detector 一次定案。

消除 bridge / benchmark 双份 ROI 路由与 Pipeline.set_detector 事后改装。
契约见 docs/design/roi-data-path.md。
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Literal

from sublift.config import DEFAULT_CONFIG
from sublift.detector.base import Detector
from sublift.detector.bottom_crop import BottomCropDetector
from sublift.detector.fixed_region import FixedRegionDetector
from sublift.detector.roi_passthrough import RoiPassthroughDetector
from sublift.extractor.ffmpeg_extractor import (
    SourceFrameInfo,
    probe_source_frame,
    validate_output_crop,
)
from sublift.models import BoundingBox

FrameOutputMode = Literal["full_rgb", "roi_rgb"]
PlanMode = Literal["auto", "full", "roi"]
TransformPolicy = Literal["fallback_full", "error"]


@dataclass(frozen=True)
class FrameIOPlan:
    """一次 job 的 extractor/detector 装配结果（构造时不变量）。"""

    output_crop: BoundingBox | None
    detector: Detector
    output_mode: FrameOutputMode
    source: SourceFrameInfo | None
    fallback_reason: str | None = None


def build_output_vf(fps: float, crop: BoundingBox | None) -> str:
    """构造 ffmpeg ``-vf`` 链。

    ROI 必须在 ``format=rgb24`` 之后 crop，与「全帧 RGB + Python crop」
    同一像素空间，避免 yuv420 色度对齐导致 detection_hash 漂移。
    """
    if crop is None:
        return f"fps={fps}"
    return f"fps={fps},format=rgb24,crop={crop.width}:{crop.height}:{crop.x}:{crop.y}:exact=1"


def plan_frame_io(
    video_path: Path,
    region_box: BoundingBox | None,
    *,
    mode: PlanMode = "auto",
    on_unvalidated_transform: TransformPolicy = "fallback_full",
    bottom_ratio: float = DEFAULT_CONFIG.region_bottom_ratio,
) -> FrameIOPlan:
    """根据 region 与模式规划 output_crop + detector。

    Args:
        video_path: 视频路径（ROI/auto 需要 probe；full/无 region 可不依赖变换）。
        region_box: source-frame 固定区；None 表示 BottomCrop 全帧路径。
        mode:
            - ``auto``：有 region 时尽量 ROI（GUI path mode）
            - ``full``：强制全帧 + FixedRegion/BottomCrop
            - ``roi``：强制 ROI；变换未验证时硬失败（benchmark）
        on_unvalidated_transform: 仅 ``mode=auto`` 时生效。
        bottom_ratio: 无 region 时 BottomCrop 比例。

    Raises:
        ValueError: 几何非法，或 mode=roi 却无 region。
        RuntimeError: mode=roi（或 auto+error）且显示变换未验证。
    """
    if mode not in ("auto", "full", "roi"):
        raise ValueError(f"mode 必须是 auto|full|roi（收到 {mode!r}）")
    if mode == "roi" and region_box is None:
        raise ValueError("mode=roi 需要 region_box")

    if region_box is None:
        return FrameIOPlan(
            output_crop=None,
            detector=BottomCropDetector(bottom_ratio=bottom_ratio),
            output_mode="full_rgb",
            source=None,
        )

    if mode == "full":
        return FrameIOPlan(
            output_crop=None,
            detector=FixedRegionDetector(region_box),
            output_mode="full_rgb",
            source=None,
        )

    # auto 或 roi：需要 source 探测
    source = probe_source_frame(video_path)
    want_roi = mode in ("auto", "roi")
    if want_roi and not source.display_transform_ok:
        if mode == "roi" or on_unvalidated_transform == "error":
            raise RuntimeError(
                "ROI 输出要求已验证的恒等显示变换，"
                f"当前未验证: note={source.transform_note!r} "
                f"source={source.width}x{source.height}"
            )
        return FrameIOPlan(
            output_crop=None,
            detector=FixedRegionDetector(region_box),
            output_mode="full_rgb",
            source=source,
            fallback_reason=source.transform_note or "unvalidated_display_transform",
        )

    validate_output_crop(region_box, source.width, source.height)
    return FrameIOPlan(
        output_crop=region_box,
        detector=RoiPassthroughDetector(region_box.width, region_box.height),
        output_mode="roi_rgb",
        source=source,
    )
