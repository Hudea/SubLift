"""Core immutable data models for tools.vlm_ocr."""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Protocol


VALID_CATEGORIES = frozenset(
    {"dialogue", "bracket_note", "watermark", "scene_text", "empty"}
)
SUBTITLE_CATEGORIES = frozenset({"dialogue", "bracket_note"})
PLACEHOLDER_CATEGORY = "placeholder"
PLACEHOLDER_TEXT = "[待补录]"


@dataclass(frozen=True)
class CropBox:
    """Pixel crop box in source-frame coordinates (x, y, width, height)."""

    x: int
    y: int
    width: int
    height: int

    def __post_init__(self) -> None:
        if self.x < 0 or self.y < 0 or self.width <= 0 or self.height <= 0:
            raise ValueError(
                f"Invalid CropBox(x={self.x}, y={self.y}, width={self.width}, height={self.height})"
            )

    @classmethod
    def parse(cls, raw: str) -> CropBox:
        """Parse 'x,y,w,h' string into CropBox."""
        parts = [p.strip() for p in raw.split(",")]
        if len(parts) != 4:
            raise ValueError(f"Expected 'x,y,w,h' crop box, got {raw!r}")
        try:
            x, y, w, h = (int(p) for p in parts)
        except ValueError as exc:
            raise ValueError(f"Non-integer coordinate in crop box {raw!r}") from exc
        return cls(x=x, y=y, width=w, height=h)

    def to_cli_arg(self) -> str:
        """Serialize to 'x,y,w,h' string for sublift_cli --region."""
        return f"{self.x},{self.y},{self.width},{self.height}"


@dataclass(frozen=True)
class TimingSlot:
    """Single subtitle timing interval produced by Stage 1."""

    slot_id: int
    index: int
    start_ms: int
    end_ms: int
    mid_ms: int
    duration_ms: int


@dataclass(frozen=True)
class TimingStreamHeader:
    """Metadata resolved before streaming Stage-1 TimingSlot items."""

    video_w: int
    video_h: int
    duration_ms: int
    fps: float
    resolved_region: CropBox
    auto_region_detected: bool
    source: str  # "external_file" | "sublift_cli_cached" | "sublift_cli_stream"


@dataclass(frozen=True)
class SheetSpec:
    """Metadata describing a rendered Contact Sheet image."""

    sheet_index: int
    sheet_name: str
    sheet_path: str
    expected_slot_ids: list[int]


@dataclass(frozen=True)
class TranscribedSlot:
    """Validated multimodal VLM transcription result for a single timing slot."""

    slot_id: int
    category: str
    lines: list[str]

    @property
    def text(self) -> str:
        return "\n".join(self.lines)


@dataclass(frozen=True)
class AssembledCue:
    """Final assembled subtitle cue ready for SRT export."""

    cue_index: int
    slot_id: int
    start_ms: int
    end_ms: int
    category: str
    text: str


@dataclass(frozen=True)
class VlmEndpointConfig:
    """Resolved OpenAI-compatible VLM endpoint configuration (redacts API key in repr)."""

    endpoint: str
    api_key: str = field(repr=False)
    model: str
    thinking_mode: str = "auto"
    timeout_s: float = 120.0
    max_retries: int = 4

    def __repr__(self) -> str:
        masked = "***" if self.api_key else ""
        return (
            f"VlmEndpointConfig(endpoint={self.endpoint!r}, api_key={masked!r}, "
            f"model={self.model!r}, thinking_mode={self.thinking_mode!r}, "
            f"timeout_s={self.timeout_s!r}, max_retries={self.max_retries!r})"
        )


@dataclass(frozen=True)
class FailedSlotRecord:
    """Audit record for an isolated single timing slot rescued by local fallback."""

    slot_id: int
    start_ms: int
    end_ms: int
    reason: str
    resolution: str
    fallback_text: str

    def to_dict(self) -> dict[str, Any]:
        return {
            "slot_id": self.slot_id,
            "start_ms": self.start_ms,
            "end_ms": self.end_ms,
            "reason": self.reason,
            "resolution": self.resolution,
            "fallback_text": self.fallback_text,
        }


class SlotFallbackResolver(Protocol):
    """Pluggable strategy invoked when an isolated single slot fails VLM transcription."""

    def resolve_failed_slot(
        self,
        slot: TimingSlot,
        strip_image: Any,
        reason: str,
    ) -> tuple[TranscribedSlot, FailedSlotRecord]:
        ...
