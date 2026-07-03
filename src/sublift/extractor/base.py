"""帧采样抽象接口。"""

from __future__ import annotations

from collections.abc import Iterator
from pathlib import Path
from typing import Protocol

from sublift.models import Frame


class Extractor(Protocol):
    """从视频按采样率抽取帧。"""

    def extract(self, video_path: Path) -> Iterator[Frame]:
        """从视频抽取帧，返回带时间戳的帧迭代器。

        Args:
            video_path: 视频文件路径。

        Yields:
            按时间顺序的 Frame。
        """
        ...
