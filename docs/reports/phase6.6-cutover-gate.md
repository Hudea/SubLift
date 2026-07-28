# Phase 6.6 Cutover Gate 验证报告

- **测试时间**：2026-07-28 21:39:11
- **Git Commit**：`6c746cc`
- **系统环境**：Darwin 26.5.2 (Python 3.12.13)
- **总体结果**：**✅ PASS**
- **已执行门禁**：correctness_parity, runtime_wall_cancel_restart_rss, gt_l3_waived
- **未执行/豁免门禁**：gt_l3_live_measurement

## 1. 正确性门禁 (Correctness Parity Gate)

| 模块/功能 | Dump 脚本 | 执行耗时 | 状态 | 详细输出 |
| :--- | :--- | :--- | :--- | :--- |
| config | `dump_config.py` | 0.173s | ✅ PASS | OK config golden matches DEFAULT_CONFIG (oracle_commit=2121326506ff647b235eac1fa22e9122e1e79a88) |
| signature | `dump_signature.py` | 0.582s | ✅ PASS | OK signature golden matches oracle (5 fixtures, oracle_commit=46952a6846f662fdc4d6c894ef56bcfdf4a5ef85) |
| changepoint | `dump_changepoint.py` | 0.472s | ✅ PASS | OK changepoint golden matches oracle (9 scenarios, oracle_commit=da19e14d4544a60fc8d5eb9204a63815980c4bab) |
| timeline | `dump_timeline.py` | 0.427s | ✅ PASS | OK timeline golden (4 scenarios) |
| dedupe | `dump_dedupe.py` | 0.425s | ✅ PASS | OK dedupe golden (10 scenarios) |
| line_select | `dump_line_select.py` | 0.432s | ✅ PASS | OK line_select golden (16 cases) |
| pipeline | `dump_pipeline.py` | 0.479s | ✅ PASS | OK pipeline golden matches oracle (8 scenarios, oracle_commit=e58f18ca2e6877a3f7cf071bdd9f74f76ac6f67d) |
| extractor | `dump_extractor.py` | 0.475s | ✅ PASS | OK extractor golden matches oracle (/Volumes/lab/pp/SubLift_CPP/benchmark/parity/goldens/extractor/extractor.v1.json) |
| vision | `dump_vision.py` | 0.398s | ✅ PASS | [OK] Vision golden check passed: /Volumes/lab/pp/SubLift_CPP/benchmark/parity/goldens/vision/vision.v1.json |
| ipc_session | `dump_ipc_session.py` | 0.298s | ✅ PASS | [OK] IPC session sequence matches golden perfectly. |

## 2. 运行时门禁 (Runtime Benchmark Gate)

| 指标项 | Python Worker | C++ Worker | 比值 (C++/Py) | 断言规则 | 结论 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Wall-time Duration** | 0.108s | 0.114s | 1.05x | $T_{cpp} \le T_{py} \times 1.30 + 0.05\text{s}$ | ✅ PASS |
| **Cancel Latency** | 35.5ms | 3.9ms | 0.11x | $L_{cancel} \le 1.0\text{s}$ | ✅ PASS |
| **Restart Latency** | 2.2ms | 2.2ms | 1.00x | $L_{restart} \le 5.0\text{s}$ | ✅ PASS |
| **Peak RSS Memory** | 110.1 MiB | 13.2 MiB | 0.12x | $RSS_{cpp} \le RSS_{py} \times 1.50$ | ✅ PASS |

> Wall 硬门为 ×1.30+0.05s；契约告警阈 ×1.10 为记录式，不单独硬失败。

## 3. GT L3 水位门禁 (Fixed-asset Quality Gate)

- **状态**：⚠️ WAIVED
- **说明**：Fixed GT video absent (Zootopia_clip_1080p.mp4 not in repo). Live L3 deferred per ADR-0022 (docs/DECISIONS.md); residual risk until asset is available on the machine.

> 豁免依据：`docs/DECISIONS.md` **ADR-0022**。 有固定 clip 的机器应去掉豁免并实测 live L3。

## 4. Cutover 结论

Cutover 门禁判定：**✅ PASS**。正确性 Parity (10 goldens) 通过; 运行时 Wall/Cancel/Restart/RSS 通过; GT L3 按 ADR-0022 豁免（非完整发布契约）。

本报告仅对**已执行**门禁判定；未执行/豁免项：gt_l3_live_measurement。不得据此声称完整发布契约（含 live GT L3）已全部满足。