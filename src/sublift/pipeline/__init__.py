"""Frozen Oracle pipeline. Do not evolve with Native product features."""

from sublift.pipeline.changepoint import (
    ChangePointDetector,
    EventType,
    State,
    StateEvent,
)
from sublift.pipeline.core import Pipeline
from sublift.pipeline.dedupe import merge_entries
from sublift.pipeline.signature import (
    FrameSignature,
    compute_dhash,
    compute_signature,
    compute_ssim,
    hamming_distance,
)
from sublift.pipeline.timeline import TimelineBuilder, TimelineSegment

__all__ = [
    "ChangePointDetector",
    "EventType",
    "FrameSignature",
    "Pipeline",
    "State",
    "StateEvent",
    "TimelineBuilder",
    "TimelineSegment",
    "compute_dhash",
    "compute_signature",
    "compute_ssim",
    "hamming_distance",
    "merge_entries",
]
