"""开发者性能模式：阶段聚合与逐字幕段 trace（feat-037）。

设计约束（见 docs/plans/phase3-opt-perf.md）：

- 默认 off：调用方不创建 recorder，热点路径只做一次 ``is None`` 判断。
- summary：有界聚合（count/total/mean/max；OCR 另有固定上限样本求 p50/p95）。
- trace：summary + 逐字幕段 JSONL（不写原始字幕文本）。
- 时钟统一 ``time.perf_counter_ns()``；毫秒仅在序列化层转换。
- extractor 等待标注为 decode+filter+RGB+stdout 输出，不得称为纯 codec decode。
"""

from __future__ import annotations

import json
import platform
import resource
import subprocess
import sys
import time
from collections.abc import Callable, Iterator
from contextlib import contextmanager
from dataclasses import dataclass, field
from enum import StrEnum
from pathlib import Path
from typing import Any, TextIO

SCHEMA_VERSION = 1

# OCR 分布样本上限：足以覆盖 Zootopia 级段数×代表帧，且内存有界。
_OCR_SAMPLE_CAP = 1024

# 已知阶段名（报告用；未列出的 stage 仍可记录，但 baseline 以这些为主）
STAGE_PROBE = "probe"
STAGE_EXTRACT_WAIT = "extract_wait"
STAGE_FRAME_MATERIALIZE = "frame_materialize"
STAGE_CROP = "crop"
STAGE_COLOR_CONVERT = "color_convert"
STAGE_SIGNATURE = "signature"
STAGE_CHANGEPOINT = "changepoint"
STAGE_OCR = "ocr"
STAGE_LINE_SELECT = "line_select"
STAGE_CLEANUP = "cleanup"
STAGE_CONSENSUS = "consensus"
STAGE_FINALIZE = "finalize"
STAGE_DEDUPE = "dedupe"
STAGE_PIPELINE_OVERHEAD = "pipeline_overhead"

# 覆盖率统计用叶子阶段（排除 finalize：其 wall 内嵌 ocr/dedupe，相加会双重计数）
_COVERAGE_LEAF_STAGES = frozenset(
    {
        STAGE_PROBE,
        STAGE_EXTRACT_WAIT,
        STAGE_FRAME_MATERIALIZE,
        STAGE_CROP,
        STAGE_COLOR_CONVERT,
        STAGE_SIGNATURE,
        STAGE_CHANGEPOINT,
        STAGE_OCR,
        STAGE_LINE_SELECT,
        STAGE_CLEANUP,
        STAGE_CONSENSUS,
        STAGE_DEDUPE,
        STAGE_PIPELINE_OVERHEAD,
    }
)

# ``pipeline_overhead`` 为 ``Pipeline.run_frames`` 的排他阶段。它覆盖帧迭代、
# 状态机编排和 recorder 调用等不属于已有叶子阶段的时间；其内部叶子必须在计算时
# 扣除，避免与既有阶段双重计数。
PIPELINE_OVERHEAD_CHILD_STAGES = _COVERAGE_LEAF_STAGES - frozenset({STAGE_PIPELINE_OVERHEAD})

EXTRACT_WAIT_LABEL = (
    "extract_wait includes codec decode + filters + RGB convert + stdout pipe output; "
    "not pure codec decode"
)
FRAME_MATERIALIZE_LABEL = "Image.frombytes() constructing PIL frames from raw RGB pipe bytes"
PIPELINE_OVERHEAD_LABEL = (
    "Pipeline run_frames exclusive wall time not covered by leaf stages "
    "(iteration, state/timeline orchestration, and recorder overhead)"
)


class PerformanceMode(StrEnum):
    """性能记录模式。"""

    OFF = "off"
    SUMMARY = "summary"
    TRACE = "trace"


def parse_performance_mode(value: str | PerformanceMode) -> PerformanceMode:
    """解析性能模式字符串。

    Raises:
        ValueError: 非法模式。
    """
    if isinstance(value, PerformanceMode):
        return value
    normalized = value.strip().lower()
    try:
        return PerformanceMode(normalized)
    except ValueError as exc:
        allowed = ", ".join(m.value for m in PerformanceMode)
        raise ValueError(f"performance.mode 必须是: {allowed}（收到 {value!r}）") from exc


def ns_to_ms(ns: int | float | None) -> float | None:
    """纳秒 → 毫秒（序列化层）。"""
    if ns is None:
        return None
    return float(ns) / 1_000_000.0


@dataclass
class StageStats:
    """单阶段有界聚合统计。"""

    count: int = 0
    total_ns: int = 0
    max_ns: int = 0
    # 仅 OCR 等需要分位数的阶段写入；帧级阶段保持空以控制内存
    samples_ns: list[int] = field(default_factory=list)
    sample_cap: int = 0

    def add(self, duration_ns: int) -> None:
        if duration_ns < 0:
            duration_ns = 0
        self.count += 1
        self.total_ns += duration_ns
        if duration_ns > self.max_ns:
            self.max_ns = duration_ns
        if self.sample_cap > 0:
            if len(self.samples_ns) < self.sample_cap:
                self.samples_ns.append(duration_ns)
            else:
                # 确定性降采样：用最新值覆盖环形槽，避免无界增长
                idx = (self.count - 1) % self.sample_cap
                self.samples_ns[idx] = duration_ns

    @property
    def mean_ns(self) -> float:
        if self.count == 0:
            return 0.0
        return self.total_ns / self.count

    def percentile_ns(self, pct: float) -> float | None:
        if not self.samples_ns:
            return None
        ordered = sorted(self.samples_ns)
        if len(ordered) == 1:
            return float(ordered[0])
        # 最近邻 rank
        rank = pct / 100.0 * (len(ordered) - 1)
        lo = int(rank)
        hi = min(lo + 1, len(ordered) - 1)
        frac = rank - lo
        return ordered[lo] * (1.0 - frac) + ordered[hi] * frac

    def to_payload(self) -> dict[str, Any]:
        payload: dict[str, Any] = {
            "count": self.count,
            "total_ms": ns_to_ms(self.total_ns),
            "mean_ms": ns_to_ms(self.mean_ns) if self.count else 0.0,
            "max_ms": ns_to_ms(self.max_ns) if self.count else 0.0,
        }
        if self.sample_cap > 0 and self.samples_ns:
            payload["p50_ms"] = ns_to_ms(self.percentile_ns(50.0))
            payload["p95_ms"] = ns_to_ms(self.percentile_ns(95.0))
        return payload


# feat-043：OCR 内部归因的可信早停原因枚举
_EARLY_STOP_SINGLE_HIGH_CONF = "single_high_confidence"
_EARLY_STOP_TWO_FRAME_CONSENSUS = "two_frame_consensus"
_EARLY_STOP_EXHAUSTED = "representative_frames_exhausted"
_EARLY_STOP_NO_VALID_SAMPLE = "no_valid_sample"
_EARLY_STOP_NO_REGION = "no_region"
_EARLY_STOP_LEGACY = "legacy_path"
_ALLOWED_EARLY_STOP_REASONS = frozenset(
    {
        _EARLY_STOP_SINGLE_HIGH_CONF,
        _EARLY_STOP_TWO_FRAME_CONSENSUS,
        _EARLY_STOP_EXHAUSTED,
        _EARLY_STOP_NO_VALID_SAMPLE,
        _EARLY_STOP_NO_REGION,
        _EARLY_STOP_LEGACY,
    }
)

# OCR 内部归因的五个子阶段名
OCR_SUB_INPUT_PREPARE = "ocr.input_prepare"
OCR_SUB_REQUEST_SETUP = "ocr.request_setup"
OCR_SUB_VISION_PERFORM = "ocr.vision_perform"
OCR_SUB_OBSERVATION_MAPPING = "ocr.observation_mapping"
OCR_SUB_RESIDUAL = "ocr.residual"

# 有界的输入几何样本上限
_OCR_GEOMETRY_SAMPLE_CAP = 256


@dataclass
class OcrCallDetail:
    """单次 OCR 调用的内部计时明细（不含文本/图像/box/路径）。

    设计契约：``total_ms ≈ input_prepare + request_setup + vision_perform
    + observation_mapping + residual``。
    """

    input_width: int
    input_height: int
    input_mode: str  # e.g. "RGB", "L"
    input_prepare_ms: float
    request_setup_ms: float
    vision_perform_ms: float
    observation_mapping_ms: float
    residual_ms: float
    total_ms: float
    outcome: str  # "success" | "empty" | "error"

    @property
    def components_ms(self) -> float:
        """五项内部阶段之和（用于与 parent total_ms 对账）。"""
        return (
            self.input_prepare_ms
            + self.request_setup_ms
            + self.vision_perform_ms
            + self.observation_mapping_ms
            + self.residual_ms
        )

    def to_dict(self) -> dict[str, Any]:
        return {
            "input_width": self.input_width,
            "input_height": self.input_height,
            "input_mode": self.input_mode,
            "input_prepare_ms": self.input_prepare_ms,
            "request_setup_ms": self.request_setup_ms,
            "vision_perform_ms": self.vision_perform_ms,
            "observation_mapping_ms": self.observation_mapping_ms,
            "residual_ms": self.residual_ms,
            "total_ms": self.total_ms,
            "outcome": self.outcome,
        }

    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> OcrCallDetail:
        return cls(
            input_width=data["input_width"],
            input_height=data["input_height"],
            input_mode=data.get("input_mode", "RGB"),
            input_prepare_ms=data.get("input_prepare_ms", 0.0),
            request_setup_ms=data.get("request_setup_ms", 0.0),
            vision_perform_ms=data.get("vision_perform_ms", 0.0),
            observation_mapping_ms=data.get("observation_mapping_ms", 0.0),
            residual_ms=data.get("residual_ms", 0.0),
            total_ms=data["total_ms"],
            outcome=data["outcome"],
        )


@dataclass
class OcrBreakdown:
    """OCR 内部归因有界聚合（summary 模式）。

    内部阶段通过各自 :class:`StageStats` 收集；但不参与 core coverage 相加。
    """

    call_count: int = 0
    call_total_ms: float = 0.0
    input_prepare: StageStats = field(
        default_factory=lambda: StageStats(sample_cap=_OCR_SAMPLE_CAP)
    )
    request_setup: StageStats = field(
        default_factory=lambda: StageStats(sample_cap=_OCR_SAMPLE_CAP)
    )
    vision_perform: StageStats = field(
        default_factory=lambda: StageStats(sample_cap=_OCR_SAMPLE_CAP)
    )
    observation_mapping: StageStats = field(
        default_factory=lambda: StageStats(sample_cap=_OCR_SAMPLE_CAP)
    )
    residual: StageStats = field(default_factory=lambda: StageStats(sample_cap=_OCR_SAMPLE_CAP))
    # (width, height, mode) → count；有上限桶
    input_geometry_buckets: dict[tuple[int, int, str], int] = field(default_factory=dict)
    # 段级决策汇总
    representative_frames_total: int = 0
    actual_ocr_calls_total: int = 0
    early_stop_reasons: dict[str, int] = field(default_factory=dict)
    accepted_count: int = 0
    rejected_count: int = 0
    engine_detail: str = "vision"  # "vision" | "opaque"

    def record_call(
        self,
        *,
        call_detail: OcrCallDetail,
    ) -> None:
        """将一次 OCR 调用的内部耗时录入本 breakdown。

        各项以纳秒整形后调用 ``StageStats.add``，几何桶以去重上限记录。
        """
        self.call_count += 1
        self.call_total_ms += call_detail.total_ms
        self.input_prepare.add(_ms_to_ns_safe(call_detail.input_prepare_ms))
        self.request_setup.add(_ms_to_ns_safe(call_detail.request_setup_ms))
        self.vision_perform.add(_ms_to_ns_safe(call_detail.vision_perform_ms))
        self.observation_mapping.add(_ms_to_ns_safe(call_detail.observation_mapping_ms))
        self.residual.add(_ms_to_ns_safe(call_detail.residual_ms))
        # 几何桶：有界去重（first-N-wins，满后永不新增）
        key = (call_detail.input_width, call_detail.input_height, call_detail.input_mode)
        buckets_full = len(self.input_geometry_buckets) >= _OCR_GEOMETRY_SAMPLE_CAP
        if key not in self.input_geometry_buckets and buckets_full:
            # 已达上限 256 种组合，新几何组合静默丢弃
            pass
        else:
            self.input_geometry_buckets[key] = self.input_geometry_buckets.get(key, 0) + 1

    def record_segment_decision(
        self,
        *,
        representative_frames: int,
        actual_ocr_calls: int,
        early_stop_reason: str,
        accepted: bool,
    ) -> None:
        """记录一个字幕段的决策信息到 breakdown 聚合。"""
        self.representative_frames_total += representative_frames
        self.actual_ocr_calls_total += actual_ocr_calls
        reason = early_stop_reason or _EARLY_STOP_EXHAUSTED
        self.early_stop_reasons[reason] = self.early_stop_reasons.get(reason, 0) + 1
        if accepted:
            self.accepted_count += 1
        else:
            self.rejected_count += 1

    def to_payload(self) -> dict[str, Any]:
        """序列化为 agent JSON 的 ``ocr_breakdown`` 块。"""
        payload: dict[str, Any] = {
            "call_count": self.call_count,
            "call_total": {
                "count": self.call_count,
                "total_ms": self.call_total_ms,
                "mean_ms": self.call_total_ms / self.call_count if self.call_count else 0.0,
            },
            "input_prepare": self.input_prepare.to_payload(),
            "request_setup": self.request_setup.to_payload(),
            "vision_perform": self.vision_perform.to_payload(),
            "observation_mapping": self.observation_mapping.to_payload(),
            "residual": self.residual.to_payload(),
            "input_geometry": [
                {"width": w, "height": h, "mode": mode, "count": cnt}
                for (w, h, mode), cnt in (
                    sorted(self.input_geometry_buckets.items(), key=lambda x: -x[1])[:64]
                )
            ],
            "segment_decisions": {
                "representative_frames_total": self.representative_frames_total,
                "actual_ocr_calls_total": self.actual_ocr_calls_total,
                "early_stop_reasons": dict(self.early_stop_reasons),
                "accepted": self.accepted_count,
                "rejected": self.rejected_count,
            },
            "accounting": self._accounting_payload(),
            "engine_detail": self.engine_detail,
        }
        return payload

    def _accounting_payload(self) -> dict[str, Any]:
        components_ms = (
            (ns_to_ms(self.input_prepare.total_ns) or 0.0)
            + (ns_to_ms(self.request_setup.total_ns) or 0.0)
            + (ns_to_ms(self.vision_perform.total_ns) or 0.0)
            + (ns_to_ms(self.observation_mapping.total_ns) or 0.0)
            + (ns_to_ms(self.residual.total_ns) or 0.0)
        )
        parent_ms = self.call_total_ms
        delta_ms = max(0.0, abs(parent_ms - components_ms))
        coverage_pct = min(100.0, components_ms / parent_ms * 100.0) if parent_ms > 0 else None
        return {
            "parent_total_ms": parent_ms,
            "components_total_ms": components_ms,
            "residual_total_ms": ns_to_ms(self.residual.total_ns) or 0.0,
            "delta_ms": delta_ms,
            "coverage_pct": coverage_pct,
            "note": "internal stages excluded from core stage coverage; "
            "parent ocr is the only coverage leaf for ocr wall",
        }


def _ms_to_ns_safe(ms: float) -> int:
    """毫秒 → 整数纳秒（非负）。"""
    return max(0, int(ms * 1_000_000.0))


@dataclass
class SegmentTraceRecord:
    """逐字幕段性能记录（不含原始字幕文本）。"""

    start_ms: int
    end_ms: int
    duration_ms: int
    representative_frames: int
    ocr_calls: int
    ocr_wall_ms: float
    select_wall_ms: float
    accepted: bool
    output_chars: int
    # feat-043a 新增
    representative_selection_ms: float = 0.0
    early_stop_reason: str = ""
    ocr_call_details: list[dict[str, Any]] = field(default_factory=list)

    def to_dict(self) -> dict[str, Any]:
        return {
            "start_ms": self.start_ms,
            "end_ms": self.end_ms,
            "duration_ms": self.duration_ms,
            "representative_frames": self.representative_frames,
            "ocr_calls": self.ocr_calls,
            "ocr_wall_ms": self.ocr_wall_ms,
            "select_wall_ms": self.select_wall_ms,
            "accepted": self.accepted,
            "output_chars": self.output_chars,
            "representative_selection_ms": self.representative_selection_ms,
            "early_stop_reason": self.early_stop_reason,
            "ocr_call_details": self.ocr_call_details,
        }


class PerformanceRecorder:
    """可选性能记录器。

    Args:
        mode: summary 或 trace（off 时不应构造本对象）。
        segment_path: trace 模式逐段 JSONL 路径；None 时仅内存收集。
        clock: 返回单调时钟纳秒的可调用对象；默认 ``time.perf_counter_ns``。
    """

    def __init__(
        self,
        mode: PerformanceMode | str = PerformanceMode.SUMMARY,
        *,
        segment_path: Path | None = None,
        clock: Callable[[], int] | None = None,
    ) -> None:
        parsed = parse_performance_mode(mode)
        if parsed is PerformanceMode.OFF:
            raise ValueError("PerformanceRecorder 不应在 mode=off 时创建")
        self._mode = parsed
        self._clock = clock or time.perf_counter_ns
        self._segment_path = segment_path
        self._segment_fh: TextIO | None = None
        self._stages: dict[str, StageStats] = {}
        self._counters: dict[str, int] = {}
        self._core_start_ns: int | None = None
        self._core_end_ns: int | None = None
        self._first_frame_ns: int | None = None
        self._first_entry_ns: int | None = None
        self._spawn_ns: int | None = None
        self._spawn_to_first_frame_ns: int | None = None
        self._completed = True
        self._incomplete_reason: str | None = None
        self._environment: dict[str, Any] = {}
        self._workload: dict[str, Any] = {}
        self._segment_records: list[SegmentTraceRecord] = []
        self._peak_rss_bytes: int = 0
        self._cpu_user_s: float = 0.0
        self._cpu_system_s: float = 0.0
        self._resources_sampled = False
        # feat-043：OCR 内部归因有界聚合
        self._ocr_breakdown: OcrBreakdown | None = None

        if self._mode is PerformanceMode.TRACE and segment_path is not None:
            segment_path.parent.mkdir(parents=True, exist_ok=True)
            self._segment_fh = segment_path.open("w", encoding="utf-8")

    @property
    def mode(self) -> PerformanceMode:
        return self._mode

    @property
    def is_trace(self) -> bool:
        return self._mode is PerformanceMode.TRACE

    def now_ns(self) -> int:
        return self._clock()

    def mark_core_start(self) -> None:
        self._core_start_ns = self._clock()

    def mark_core_end(self) -> None:
        self._core_end_ns = self._clock()

    def mark_spawn(self) -> None:
        self._spawn_ns = self._clock()

    def mark_first_frame(self) -> None:
        now = self._clock()
        if self._first_frame_ns is None:
            self._first_frame_ns = now
        if self._spawn_ns is not None and self._spawn_to_first_frame_ns is None:
            self._spawn_to_first_frame_ns = now - self._spawn_ns

    def mark_first_entry(self) -> None:
        if self._first_entry_ns is None:
            self._first_entry_ns = self._clock()

    def note_incomplete(self, reason: str) -> None:
        self._completed = False
        self._incomplete_reason = reason

    def incr(self, name: str, amount: int = 1) -> None:
        self._counters[name] = self._counters.get(name, 0) + amount

    def set_counter(self, name: str, value: int) -> None:
        self._counters[name] = value

    def add_stage_ns(self, stage: str, duration_ns: int, *, sample: bool = False) -> None:
        stats = self._stages.get(stage)
        if stats is None:
            cap = _OCR_SAMPLE_CAP if sample or stage == STAGE_OCR else 0
            stats = StageStats(sample_cap=cap)
            self._stages[stage] = stats
        stats.add(duration_ns)

    @contextmanager
    def span(self, stage: str, *, sample: bool = False) -> Iterator[None]:
        """记录命名阶段 wall time。异常时仍计入已消耗时间。"""
        start = self._clock()
        try:
            yield
        finally:
            self.add_stage_ns(stage, self._clock() - start, sample=sample)

    @contextmanager
    def exclusive_span(self, stage: str, *, child_stages: frozenset[str]) -> Iterator[None]:
        """记录扣除指定子阶段后的排他 wall time。

        ``child_stages`` 必须是彼此不重叠的叶子阶段。该 API 适合为外层编排
        路径补齐 coverage：外层 wall 先完整计时，再扣除其中已有的叶子 span，
        从而不把 OCR、crop 等已有成本重复计入。
        """
        start = self._clock()
        before = {name: self._stages.get(name, StageStats()).total_ns for name in child_stages}
        try:
            yield
        finally:
            elapsed_ns = self._clock() - start
            child_ns = sum(
                max(0, self._stages.get(name, StageStats()).total_ns - total_ns)
                for name, total_ns in before.items()
            )
            self.add_stage_ns(stage, max(0, elapsed_ns - child_ns))

    def record_segment(
        self,
        *,
        start_ms: int,
        end_ms: int,
        representative_frames: int,
        ocr_calls: int,
        ocr_wall_ns: int,
        select_wall_ns: int,
        accepted: bool,
        output_chars: int,
        # feat-043a 新增
        representative_selection_ms: float = 0.0,
        early_stop_reason: str = "",
        ocr_call_details: list[dict[str, Any]] | None = None,
    ) -> None:
        """记录逐字幕段成本。summary 模式忽略；trace 写入 JSONL/内存。

        feat-043a 新增 representative_selection_ms、early_stop_reason
        与 ocr_call_details，写入 trace JSONL 且不计入 core coverage。
        """
        if self._mode is not PerformanceMode.TRACE:
            return
        rec = SegmentTraceRecord(
            start_ms=start_ms,
            end_ms=end_ms,
            duration_ms=max(0, end_ms - start_ms),
            representative_frames=representative_frames,
            ocr_calls=ocr_calls,
            ocr_wall_ms=ns_to_ms(ocr_wall_ns) or 0.0,
            select_wall_ms=ns_to_ms(select_wall_ns) or 0.0,
            accepted=accepted,
            output_chars=output_chars,
            representative_selection_ms=representative_selection_ms,
            early_stop_reason=early_stop_reason,
            ocr_call_details=list(ocr_call_details) if ocr_call_details else [],
        )
        self._segment_records.append(rec)
        if self._segment_fh is not None:
            self._segment_fh.write(json.dumps(rec.to_dict(), ensure_ascii=False) + "\n")
            self._segment_fh.flush()

    def sample_resources(self) -> None:
        """采样当前进程资源水位（峰值 RSS / CPU 秒）。"""
        usage = resource.getrusage(resource.RUSAGE_SELF)
        rss = int(usage.ru_maxrss)
        # Linux: KB；macOS: bytes
        if sys.platform.startswith("linux"):
            rss_bytes = rss * 1024
            unit = "linux_kb_scaled_to_bytes"
        else:
            rss_bytes = rss
            unit = "macos_bytes"
        if rss_bytes > self._peak_rss_bytes:
            self._peak_rss_bytes = rss_bytes
        self._cpu_user_s = float(usage.ru_utime)
        self._cpu_system_s = float(usage.ru_stime)
        self._resources_sampled = True
        self._environment.setdefault("rss_unit", unit)

    @property
    def ocr_breakdown(self) -> OcrBreakdown | None:
        """OCR 内部归因聚合（summary/trace 均可用；off 时为 None）。"""
        return self._ocr_breakdown

    def ensure_ocr_breakdown(self, *, engine_detail: str = "vision") -> OcrBreakdown:
        """获取或创建 OCR breakdown；首次调用设定 engine_detail。

        幂等：多次调用返回同一实例；engine_detail 仅首次写入。
        """
        if self._ocr_breakdown is None:
            self._ocr_breakdown = OcrBreakdown(engine_detail=engine_detail)
        elif engine_detail != "vision" and self._ocr_breakdown.engine_detail == "vision":
            self._ocr_breakdown.engine_detail = engine_detail
        return self._ocr_breakdown

    def add_ocr_call_detail(self, detail: OcrCallDetail) -> None:
        """记录一次 OCR 调用的内部计时明细到 breakdown。

        调用方（Pipeline/Vision）负责每个 OCR recognize 调用后触达。
        summary/trace 模式下均聚合；不影响 core coverage。
        """
        bd = self._ocr_breakdown
        if bd is not None:
            bd.record_call(call_detail=detail)

    def record_segment_decision(
        self,
        *,
        representative_frames: int,
        actual_ocr_calls: int,
        early_stop_reason: str,
        accepted: bool,
    ) -> None:
        """记录段级决策到 OCR breakdown（summary/trace 通用）。"""
        bd = self._ocr_breakdown
        if bd is None:
            return
        bd.record_segment_decision(
            representative_frames=representative_frames,
            actual_ocr_calls=actual_ocr_calls,
            early_stop_reason=early_stop_reason,
            accepted=accepted,
        )

    def set_environment(self, env: dict[str, Any]) -> None:
        self._environment.update(env)

    def set_workload(self, workload: dict[str, Any]) -> None:
        self._workload.update(workload)

    def close(self) -> None:
        if self._segment_fh is not None:
            self._segment_fh.close()
            self._segment_fh = None

    def __enter__(self) -> PerformanceRecorder:
        return self

    def __exit__(self, *args: object) -> None:
        self.close()

    def _latency_ms(self, event_ns: int | None) -> float | None:
        if event_ns is None or self._core_start_ns is None:
            return None
        return ns_to_ms(event_ns - self._core_start_ns)

    def core_wall_ms(self) -> float | None:
        if self._core_start_ns is None:
            return None
        end = self._core_end_ns if self._core_end_ns is not None else self._clock()
        return ns_to_ms(end - self._core_start_ns)

    def _attribution(
        self, core_ms: float | None
    ) -> tuple[float | None, float | None, float | None]:
        """叶子阶段合计、未归因 ms、覆盖率 %。

        排除 ``finalize`` 容器 span，避免与其内部 ocr/dedupe 双重计数。
        """
        # 仅合计叶子阶段；finalize 为容器不计入（内部 ocr/dedupe 已单独计）
        attributed_ns = sum(
            stats.total_ns for name, stats in self._stages.items() if name in _COVERAGE_LEAF_STAGES
        )
        attributed_ms = ns_to_ms(attributed_ns)
        if core_ms is None or attributed_ms is None:
            return attributed_ms, None, None
        unattributed = max(0.0, float(core_ms) - float(attributed_ms))
        coverage = (
            min(100.0, float(attributed_ms) / float(core_ms) * 100.0) if core_ms > 0 else None
        )
        return attributed_ms, unattributed, coverage

    def to_payload(self) -> dict[str, Any]:
        """序列化为 agent JSON 的 ``performance`` 块（单次 run）。"""
        if not self._resources_sampled:
            self.sample_resources()

        stages = {name: stats.to_payload() for name, stats in sorted(self._stages.items())}
        # 为 extract_wait / frame_materialize 附加口径说明
        if STAGE_EXTRACT_WAIT in stages:
            stages[STAGE_EXTRACT_WAIT]["definition"] = EXTRACT_WAIT_LABEL
        if STAGE_FRAME_MATERIALIZE in stages:
            stages[STAGE_FRAME_MATERIALIZE]["definition"] = FRAME_MATERIALIZE_LABEL
        if STAGE_PIPELINE_OVERHEAD in stages:
            stages[STAGE_PIPELINE_OVERHEAD]["definition"] = PIPELINE_OVERHEAD_LABEL

        core_ms = self.core_wall_ms()
        video_duration_s = self._workload.get("video_duration_seconds")
        realtime_factor = None
        if (
            core_ms is not None
            and isinstance(video_duration_s, int | float)
            and core_ms > 0
            and video_duration_s > 0
        ):
            realtime_factor = float(video_duration_s) / (core_ms / 1000.0)

        attributed_ms, unattributed_ms, coverage_pct = self._attribution(core_ms)

        payload: dict[str, Any] = {
            "schema_version": SCHEMA_VERSION,
            "mode": self._mode.value,
            "completed": self._completed,
            "incomplete_reason": self._incomplete_reason,
            "environment": dict(self._environment),
            "workload": dict(self._workload),
            "latency": {
                "first_frame_ms": self._latency_ms(self._first_frame_ns),
                "first_entry_ms": self._latency_ms(self._first_entry_ns),
                "spawn_to_first_frame_ms": ns_to_ms(self._spawn_to_first_frame_ns),
            },
            "throughput": {
                "core_wall_ms": core_ms,
                "realtime_factor": realtime_factor,
                "frame_count": self._counters.get("frame_count", 0),
                "raw_output_bytes": self._counters.get("raw_output_bytes", 0),
                "raw_bytes_per_frame": self._workload.get("raw_bytes_per_frame"),
                "source_width": self._workload.get("source_width"),
                "source_height": self._workload.get("source_height"),
                "output_width": self._workload.get("output_width"),
                "output_height": self._workload.get("output_height"),
                "output_mode": self._workload.get("output_mode"),
                "source_region_box": self._workload.get("source_region_box"),
                "full_frame_passthrough_count": self._counters.get(
                    "full_frame_passthrough_count", 0
                ),
                "pipeline_crop_count": self._counters.get("pipeline_crop_count", 0),
                "ocr_calls": self._stages.get(STAGE_OCR, StageStats()).count,
                "attributed_stage_ms": attributed_ms,
                "unattributed_ms": unattributed_ms,
                "stage_coverage_pct": coverage_pct,
            },
            "resources": {
                "python_peak_rss_bytes": self._peak_rss_bytes,
                "cpu_user_seconds": self._cpu_user_s,
                "cpu_system_seconds": self._cpu_system_s,
            },
            "stages": stages,
            "counters": dict(self._counters),
        }
        if self._mode is PerformanceMode.TRACE:
            payload["segment_count"] = len(self._segment_records)
            if self._segment_path is not None:
                payload["segment_trace_path"] = str(self._segment_path)
        # feat-043：OCR 内部归因（独立块，不参与 core coverage 相加）
        if self._ocr_breakdown is not None and (
            self._ocr_breakdown.call_count > 0
            or self._ocr_breakdown.accepted_count + self._ocr_breakdown.rejected_count > 0
        ):
            payload["ocr_breakdown"] = self._ocr_breakdown.to_payload()
        return payload


def collect_environment(*, repo_root: Path | None = None) -> dict[str, Any]:
    """采集运行环境元数据（commit / OS / CPU / Python / ffmpeg）。"""
    env: dict[str, Any] = {
        "platform": platform.platform(),
        "system": platform.system(),
        "machine": platform.machine(),
        "processor": platform.processor() or platform.machine(),
        "python_version": sys.version.split()[0],
        "python_implementation": platform.python_implementation(),
    }
    env.update(_git_snapshot(repo_root))
    env["ffmpeg_version"] = _ffmpeg_version()
    env["cpu_count"] = _cpu_count()
    try:
        from sublift.ocr import is_vision_available

        env["vision_available"] = bool(is_vision_available())
    except Exception:
        env["vision_available"] = False
    return env


def _git_snapshot(repo_root: Path | None) -> dict[str, Any]:
    cwd = str(repo_root) if repo_root is not None else None
    commit = _run_git(["rev-parse", "HEAD"], cwd=cwd)
    dirty = _run_git(["status", "--porcelain"], cwd=cwd)
    return {
        "git_commit": commit or None,
        "git_dirty": bool(dirty) if dirty is not None else None,
    }


def _run_git(args: list[str], *, cwd: str | None) -> str | None:
    try:
        result = subprocess.run(
            ["git", *args],
            capture_output=True,
            text=True,
            check=False,
            cwd=cwd,
            timeout=5,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    if result.returncode != 0:
        return None
    return result.stdout.strip()


def _ffmpeg_version() -> str | None:
    try:
        result = subprocess.run(
            ["ffmpeg", "-version"],
            capture_output=True,
            text=True,
            check=False,
            timeout=5,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    if result.returncode != 0 or not result.stdout:
        return None
    first = result.stdout.splitlines()[0].strip()
    return first or None


def _cpu_count() -> int | None:
    import os

    return os.cpu_count()


def aggregate_run_payloads(
    runs: list[dict[str, Any]],
    *,
    primary: str = "median",
) -> dict[str, Any]:
    """聚合多次 measured run 的 performance payload。

    对 core_wall_ms / realtime_factor / peak RSS 等标量取 median/min/max。
    """
    if not runs:
        return {"primary": primary, "measured_runs": 0, "runs": []}

    def _values(key_path: tuple[str, ...]) -> list[float]:
        out: list[float] = []
        for run in runs:
            cur: Any = run
            for key in key_path:
                if not isinstance(cur, dict) or key not in cur:
                    cur = None
                    break
                cur = cur[key]
            if isinstance(cur, int | float) and cur is not None:
                out.append(float(cur))
        return out

    def _stat(vals: list[float]) -> dict[str, float | None]:
        if not vals:
            return {"median": None, "min": None, "max": None}
        ordered = sorted(vals)
        mid = len(ordered) // 2
        med = ordered[mid] if len(ordered) % 2 else (ordered[mid - 1] + ordered[mid]) / 2.0
        return {"median": med, "min": ordered[0], "max": ordered[-1]}

    return {
        "primary": primary,
        "measured_runs": len(runs),
        "core_wall_ms": _stat(_values(("throughput", "core_wall_ms"))),
        "realtime_factor": _stat(_values(("throughput", "realtime_factor"))),
        "first_frame_ms": _stat(_values(("latency", "first_frame_ms"))),
        "first_entry_ms": _stat(_values(("latency", "first_entry_ms"))),
        "spawn_to_first_frame_ms": _stat(_values(("latency", "spawn_to_first_frame_ms"))),
        "python_peak_rss_bytes": _stat(_values(("resources", "python_peak_rss_bytes"))),
        "raw_output_bytes": _stat(_values(("throughput", "raw_output_bytes"))),
        "ocr_calls": _stat(_values(("throughput", "ocr_calls"))),
        "stage_total_ms": {
            stage: _stat(
                [
                    float(run["stages"][stage]["total_ms"])
                    for run in runs
                    if isinstance(run.get("stages"), dict)
                    and stage in run["stages"]
                    and isinstance(run["stages"][stage], dict)
                    and isinstance(run["stages"][stage].get("total_ms"), int | float)
                ]
            )
            for stage in sorted(
                {
                    stage
                    for run in runs
                    if isinstance(run.get("stages"), dict)
                    for stage in run["stages"]
                }
            )
        },
    }
