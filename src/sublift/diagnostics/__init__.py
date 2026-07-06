"""打轴诊断工具：trace 记录与 FN 归因。

本模块为可选调试能力，对 pipeline 主流程零影响。通过注入
``TraceRecorder`` 到 ``ChangePointDetector`` 启用逐帧决策记录，再用
``classify_fn`` 对漏检条目做归因。
"""

from sublift.diagnostics.fn_analysis import (
    DetectedSegment,
    FnClassification,
    classify_fn,
    format_fn_report,
)
from sublift.diagnostics.short_subtitle import (
    ShortSubtitleMetrics,
    compute_short_subtitle_metrics,
    format_short_subtitle_report,
)
from sublift.diagnostics.trace import (
    TraceRecord,
    TraceRecorder,
    TriggerReason,
    VetoReason,
    load_jsonl,
)

__all__ = [
    "DetectedSegment",
    "FnClassification",
    "ShortSubtitleMetrics",
    "TraceRecord",
    "TraceRecorder",
    "TriggerReason",
    "VetoReason",
    "classify_fn",
    "compute_short_subtitle_metrics",
    "format_fn_report",
    "format_short_subtitle_report",
    "load_jsonl",
]
