# Diagnostic scripts

面向算法定位的低频脚本，不是 benchmark 主入口：

- `run_timeline.py`：查看变化点与时间轴；
- `run_trace.py`：记录并比较打轴决策 trace；
- `scan_params.py`：历史短字幕专项参数扫描；
- `ocr_compare.py`：逐时段 OCR 人工对照。

端到端质量、性能、参数矩阵和已有 SRT 评分统一使用：

```bash
uv run sublift-benchmark --help
```

这些诊断脚本可以读取 benchmark 数据，但不负责标准报告、运行目录或回归门。
