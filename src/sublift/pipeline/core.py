"""端到端编排：extractor → detector → ocr → timeline → dedupe。"""

from __future__ import annotations


class Pipeline:
    """字幕提取流水线编排器。

    组合 extractor/detector/ocr，产出 SubtitleEntry 列表。
    实现见 feat-015。
    """

    def __init__(self) -> None:
        raise NotImplementedError("Pipeline 实现见 feat-015")
