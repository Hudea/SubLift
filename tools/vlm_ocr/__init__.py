"""Standalone multimodal VLM subtitle OCR & algorithm validation tool for SubLift."""

__version__ = "1.0.0"

from .models import (
    AssembledCue,
    CropBox,
    SheetSpec,
    TimingSlot,
    TranscribedSlot,
    VlmEndpointConfig,
)

__all__ = [
    "__version__",
    "AssembledCue",
    "CropBox",
    "SheetSpec",
    "TimingSlot",
    "TranscribedSlot",
    "VlmEndpointConfig",
]
