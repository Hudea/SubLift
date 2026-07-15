"""SubLift 端到端 Benchmark 框架。

定义当前诊断指标：打轴 timing、识别 recognition、端到端可用性 e2e、
处理速度，以及 agent 可读 JSON/CSV/Markdown 产物；可选性能模式见
``RunConfig.performance_mode``。

用法：`benchmark/README.md`
设计：`docs/design/benchmark.md`
脚本入口：``uv run python scripts/run_benchmark_manifest.py <manifest.json>``

模块：
- ``srt_loader``：SRT 解析（统一 ground truth 与 pipeline 输出的加载）。
- ``diagnostics``：一对一匹配、case 分类、诊断指标与报告 payload。
- ``runner``：编排「视频 + GT → pipeline → 报告」，以及 ``align_existing_srt`` 复现历史产物。
- ``manifest``：JSON 配方 → ``RunConfig``。
- ``report``：agent JSON / GT CSV / detection CSV / summary Markdown 输出。
"""

from benchmark.manifest import ManifestError, load_run_config
from benchmark.runner import RunConfig, RunResult, align_existing_srt, run_benchmark

__all__ = [
    "ManifestError",
    "RunConfig",
    "RunResult",
    "align_existing_srt",
    "load_run_config",
    "run_benchmark",
]
