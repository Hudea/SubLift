"""打轴决策 trace 记录。

逐帧记录状态机决策上下文，用于 FN 归因与算法调试。默认不启用，对
pipeline 主流程零影响（通过 ``ChangePointDetector.trace_recorder`` 注入）。

记录维度：
- 帧签名信号（timestamp_ms / foreground_ratio / has_subtitle / dhash）
- 与锚帧的距离（anchor_dhash / distance）
- 状态机内部状态（state / candidate_change_ms）
- 产出事件（event_type）
- 决策原因（trigger_reason / veto_reason）

写入策略由 ``TraceRecorder`` 决定：内存 list 用于单元测试，JSONL 文件
用于真实视频调试。调用方在 pipeline 结束后通过 ``records`` 读取或
``flush()`` 落盘。
"""

from __future__ import annotations

import json
from dataclasses import asdict, dataclass, field
from enum import Enum
from pathlib import Path
from typing import Any, TextIO


class TriggerReason(Enum):
    """事件触发原因。"""

    PRESENCE_RISE = "presence_rise"
    """前景占比上升达到阈值（IN 事件）。"""

    PRESENCE_FALL = "presence_fall"
    """前景占比下降低于阈值（OUT 事件）。"""

    DHASH_EXCEEDS = "dhash_exceeds"
    """与锚帧 dHash 距离超过阈值（CHANGE 候选）。"""

    SSIM_PATROL = "ssim_patrol"
    """SSIM 巡逻检测到结构变化（CHANGE 候选，feat-031b）。"""

    NONE = "none"
    """本帧未产生候选。"""


class VetoReason(Enum):
    """候选否决原因（候选产生但未确认为事件）。"""

    NONE = "none"
    """无否决（候选已确认或无候选）。"""

    DISTANCE_BELOW_THRESHOLD = "distance_below_threshold"
    """dHash 距离未超阈值，未产生候选。"""

    SSIM_VETOED = "ssim_vetoed"
    """SSIM 验证相似度高，否决 dHash 候选。"""

    UNSTABLE_CONTENT = "unstable_content"
    """新内容未稳定（与上一帧距离仍超阈值），未确认。"""

    HYSTERESIS_NOT_MET = "hysteresis_not_met"
    """迟滞帧数未达，状态未迁移。"""


@dataclass
class TraceRecord:
    """单帧打轴决策记录。

    Attributes:
        timestamp_ms: 帧时间戳（毫秒）。
        foreground_ratio: 前景像素占比。
        has_subtitle: 前景占比是否达到 presence_threshold。
        dhash: 当前帧 dHash。
        anchor_dhash: 锚帧 dHash（无锚帧时为 None）。
        distance: 当前帧与锚帧 dHash 汉明距离（无锚帧时为 None）。
        ssim: 可选 SSIM 值（patrol 启用时填充）。
        state: 本帧处理后的状态机状态名称。
        event_type: 本帧产出的事件类型名称（无事件为 None）。
        candidate_change_ms: 当前待确认的 CHANGE 候选时间戳（无候选为 None）。
        trigger_reason: 候选触发原因。
        veto_reason: 候选否决原因。
    """

    timestamp_ms: int
    foreground_ratio: float
    has_subtitle: bool
    dhash: int
    anchor_dhash: int | None
    distance: int | None
    ssim: float | None
    state: str
    event_type: str | None
    candidate_change_ms: int | None
    trigger_reason: str
    veto_reason: str


@dataclass
class TraceRecorder:
    """打轴决策 trace 收集器。

    用法：
        recorder = TraceRecorder()
        detector = ChangePointDetector(trace_recorder=recorder)
        # ... 跑 pipeline ...
        for record in recorder.records:
            ...

    或写入文件：
        recorder = TraceRecorder(jsonl_path=Path("trace.jsonl"))
        # ... 跑 pipeline ...
        recorder.flush()  # 关闭文件
    """

    jsonl_path: Path | None = None
    """可选 JSONL 文件路径。设置后每条记录实时写入文件。"""

    records: list[TraceRecord] = field(default_factory=list, init=False)
    """内存记录缓存（即使设置了 jsonl_path 也会保留）。"""

    _file: TextIO | None = field(default=None, init=False, repr=False)

    def __post_init__(self) -> None:
        if self.jsonl_path is not None:
            self.jsonl_path.parent.mkdir(parents=True, exist_ok=True)
            self._file = self.jsonl_path.open("a", encoding="utf-8")

    def record(
        self,
        timestamp_ms: int,
        foreground_ratio: float,
        has_subtitle: bool,
        dhash: int,
        anchor_dhash: int | None,
        distance: int | None,
        state: str,
        event_type: str | None,
        candidate_change_ms: int | None,
        trigger_reason: TriggerReason,
        veto_reason: VetoReason,
        ssim: float | None = None,
    ) -> None:
        """记录一帧的决策上下文。"""
        rec = TraceRecord(
            timestamp_ms=timestamp_ms,
            foreground_ratio=foreground_ratio,
            has_subtitle=has_subtitle,
            dhash=dhash,
            anchor_dhash=anchor_dhash,
            distance=distance,
            ssim=ssim,
            state=state,
            event_type=event_type,
            candidate_change_ms=candidate_change_ms,
            trigger_reason=trigger_reason.value,
            veto_reason=veto_reason.value,
        )
        self.records.append(rec)
        if self._file is not None:
            self._file.write(json.dumps(_record_to_dict(rec), ensure_ascii=False) + "\n")
            self._file.flush()

    def flush(self) -> None:
        """关闭底层文件（如有）。"""
        if self._file is not None:
            self._file.close()
            self._file = None

    def clear(self) -> None:
        """清空内存记录（不影响已写入文件）。"""
        self.records.clear()


def _record_to_dict(rec: TraceRecord) -> dict[str, Any]:
    """TraceRecord → JSON 友好的 dict。"""
    d = asdict(rec)
    return d


def load_jsonl(path: Path) -> list[TraceRecord]:
    """从 JSONL 文件加载 trace 记录（用于离线分析）。"""
    records: list[TraceRecord] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line:
            continue
        data = json.loads(line)
        records.append(TraceRecord(**data))
    return records
