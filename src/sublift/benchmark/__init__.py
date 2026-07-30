"""SubLift 端到端 Benchmark 框架。

定义当前诊断指标：打轴 timing、识别 recognition、端到端可用性 e2e、
处理速度，以及 agent 可读 JSON/CSV/Markdown 产物；可选性能模式见
``RunConfig.performance_mode``。

用法：`benchmark/README.md`
设计：`docs/design/benchmark.md`
统一入口：``uv run sublift-benchmark``

模块：
- ``srt``：SRT 解析（统一 ground truth 与 pipeline 输出的加载）。
- ``diagnostics``：一对一匹配、case 分类、诊断指标与报告 payload。
- ``runner``：编排「视频 + GT → pipeline → 报告」，以及 ``align_existing_srt`` 复现历史产物。
- ``config``：v1/v2 配方、矩阵与 CLI override → ``RunConfig``。
- ``report``：agent JSON / GT CSV / detection CSV / summary Markdown 输出。
"""

from sublift.benchmark.config import (
    ManifestDocument,
    ManifestError,
    RunConfig,
    load_manifest,
    load_run_config,
    resolve_run_config,
)
from sublift.benchmark.runner import RunResult, align_existing_srt, run_benchmark

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
