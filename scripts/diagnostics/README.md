# Diagnostic scripts

面向低频定位的脚本，不是 benchmark 主入口，也不替代默认 C++ Worker 的产品门。
当前身份以 `docs/TESTMAP.md` §3.4 为准。

**仍可跑（需显式媒体/输出参数）：**

- `run_timeline.py`：查看变化点与时间轴；
- `run_trace.py`：记录并比较打轴决策 trace；
- `scan_params.py`：历史短字幕专项参数扫描；
- `ocr_compare.py`：逐时段 OCR 人工对照；
- `extract_frames.py`：从显式指定的视频抽取有限帧；
- `compare_detectors.py`：输出 BottomCrop 与 FixedRegion 的裁剪结果以供人工比较。

**已删除：** `audit_python_ipc.py` / `long_video_ux.py`。请用 Native Worker / `sublift extract`，历史实现见 tag `python-product-last`。

**不在本目录：** `scripts/compare_roi_ab.py` 等三个根目录脚本是 `sublift-benchmark` 的 historical shim，不是诊断套件。

端到端质量、性能、参数矩阵和已有 SRT 评分统一使用：

```bash
uv run sublift-benchmark --help
```

这些诊断脚本可以读取 benchmark 数据，但不负责标准报告、运行目录或回归门。

## 原 Phase 9 根目录脚本处置表

下表记录 09003 对历史根目录手工入口的逐项审计。历史 Phase evidence 中的原始命令
保留为当时事实；不以当前路径改写。所有保留脚本均要求显式媒体路径，输出仅写到调用方
指定的目录或标准输出。原 Phase 9 已按 ADR-0036 归入 Phase 7；09003 的当前记录位于
`docs/phases/phase7.json`。

| 原根入口 | 职责、输入与输出 | 当前文档 / Phase 引用 | 自动覆盖 | 独有验收价值 | 处置与结论证据 |
| --- | --- | --- | --- | --- | --- |
| `audit_memory_push.py` | Python UDS 的真实视频 start/cancel/restart；输入视频，输出 RSS / 推送时机 | 仅 Phase 9 计划 | `tests/test_ipc_bridge.py`、`tests/test_ipc_server.py`、`tests/ipc/test_cpp_worker.py` 覆盖协议取消；不测 Python 子进程 RSS 与 ffmpeg 退出 | 有：实际 Python Oracle 进程的 RSS、子进程退出和首条推送 | 迁移为 `audit_python_ipc.py`，显式 `--video` / `--engine` / `--socket`；标明不代表默认 C++ 产品门 |
| `extract_frames.py` | 输入视频，输出有限 PNG 帧 | 仅 Phase 9 计划 | 历史由已移除的 Oracle 抽帧单测覆盖；产品抽帧由 `cpp/tests` extractor 用例覆盖 | 有：人工查看原始抽帧 | 迁移为 `extract_frames.py`，要求 `--video --out`，取消硬编码媒体和输出目录 |
| `feat040_long_video_ux.py` | 长视频 path-mode 取消、重启、导出、进度与 RSS；输出 JSON/SRT | Phase 4 historical evidence、Phase 9 计划 | 历史 Python IPC/export 单测已移除；产品取消/导出由 Native Worker 与 `cpp/tests` 覆盖 | 有：同一 Bridge 上的真实长视频 UX 与资源诊断 | 迁移为 `long_video_ux.py`；保留显式 `--video`，默认输出改为 `debug/diagnostics/long-video-ux` |
| `test_cancel.py` | Python Pipeline 处理 50 帧后取消，打印计数 | 仅 Phase 9 计划 | 历史 Oracle Pipeline 取消单测已移除；产品取消由 Native Worker / CTest 覆盖 | 无：只重复当时取消状态断言，且未断言退出码 | 删除当前入口 |
| `test_cli_e2e.py` | 固定 Zootopia 视频/SRT 的 CLI 提取与时间、CER 报告 | Phase 1 historical evidence、Phase 9 计划 | `tests/test_cli.py`；`tests/test_benchmark_matrix.py::test_cli_score_existing_srt`；canonical `sublift-benchmark run/score` | 无：硬编码旧输入和临时文本报告，当前统一评分更完整 | 删除当前入口；历史 evidence 保留原命令 |
| `test_detectors.py` | 固定视频的两种检测器裁剪 PNG | 仅 Phase 9 计划 | 历史 Oracle detector/extractor 单测已移除；产品检测由 `cpp/tests` 覆盖 | 有：视觉比较裁剪，而非单元断言 | 迁移为 `compare_detectors.py`，要求 `--video --out`，区域与帧数可配置 |
| `test_export_e2e.py` | Python Pipeline 导出并与固定 SRT 计算时间/文本报告 | Phase 1 historical evidence、Phase 9 计划 | `tests/test_benchmark_diagnostics.py`、canonical `sublift-benchmark score`；历史 Oracle export 单测已移除 | 无：固定样本的 ad hoc 评分已被统一诊断口径覆盖 | 删除当前入口 |
| `test_pipeline_e2e.py` | Python Pipeline 与固定 SRT 的时间轴对照报告 | Phase 1 historical evidence、Phase 9 计划 | `tests/test_benchmark_diagnostics.py`、canonical `sublift-benchmark run/score`；历史 Oracle pipeline 单测已移除 | 无：固定样本报告未增加当前质量门 | 删除当前入口 |
| `test_streaming.py` | 真实视频比较批量与流式 Python Pipeline，打印首条时间 | 仅 Phase 9 计划 | 历史 Oracle/IPC 流式单测已移除；产品流式与取消由 Native Worker / CTest 覆盖 | 无：流式一致性、推送和取消已有 Native 覆盖 | 删除当前入口 |

保留脚本是诊断工具，不进入 `scripts/verify-standard.sh`。端到端质量、性能、参数矩阵和
已有 SRT 评分仍只通过 canonical CLI：

```bash
uv run sublift-benchmark --help
```
