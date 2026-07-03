"""SubLift 默认配置。"""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class Config:
    """提取流程默认配置。"""

    sample_fps: float = 1.0
    region_bottom_ratio: float = 0.3
    confidence_threshold: float = 0.5
    merge_gap_ms: int = 1000
    min_duration_ms: int = 500


DEFAULT_CONFIG = Config()
