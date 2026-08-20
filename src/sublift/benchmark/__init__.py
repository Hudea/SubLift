"""SubLift 端到端 Benchmark 框架。

定义当前诊断指标：打轴 timing、识别 recognition、端到端可用性 e2e、
处理速度，以及 agent 可读 JSON/CSV/Markdown 产物；可选性能模式见
``RunConfig.performance_mode``。

用法：`benchmark/README.md`
设计：`docs/design/benchmark.md`
统一入口：``uv run sublift-benchmark`` / ``python -m sublift_offline``

模块：
- ``srt``：SRT 解析（统一 ground truth 与 detection 输出的加载）。
- ``diagnostics``：一对一匹配、case 分类、诊断指标与报告 payload。
- ``score`` / ``report``：已有 SRT 评分与报告；导入不加载 Pipeline/OCR。
- ``runner``：默认 Native CLI 提取；``backend=oracle`` 才加载冻结 Python Pipeline。
- ``config``：v1/v2 配方、矩阵与 CLI override → ``RunConfig``。
"""

from typing import Any

from sublift.benchmark.config import (
    ManifestDocument,
    ManifestError,
    RunConfig,
    load_manifest,
    load_run_config,
    resolve_run_config,
)
from sublift.benchmark.result import RunResult
from sublift.benchmark.score import align_existing_srt

__all__ = [
    "ManifestDocument",
    "ManifestError",
    "RunConfig",
    "RunResult",
    "align_existing_srt",
    "load_manifest",
    "load_run_config",
    "resolve_run_config",
    "run_benchmark",
]


def __getattr__(name: str) -> Any:
    """Load the extract runner only when a caller asks for it."""
    if name == "run_benchmark":
        from sublift.benchmark.runner import run_benchmark

        return run_benchmark
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
