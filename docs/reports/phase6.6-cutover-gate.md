# Phase 6.6 Cutover Gate 验证报告

- **测试时间**：2026-07-29 22:45:00
- **Git Commit**：`6f58e52`
- **系统环境**：Darwin 26.5.2 (Python 3.12.13)
- **总体结果**：**✅ PASS**
- **已执行门禁**：correctness_parity, runtime_wall_cancel_restart_rss, gt_l3_measured
- **未执行/豁免门禁**：无

## 1. 正确性门禁 (Correctness Parity Gate)

| 模块/功能 | Dump 脚本 | 执行耗时 | 状态 | 详细输出 |
| :--- | :--- | :--- | :--- | :--- |
| config | `dump_config.py` | 0.215s | ✅ PASS | OK config golden matches DEFAULT_CONFIG (oracle_commit=2121326506ff647b235eac1fa22e9122e1e79a88) |
| signature | `dump_signature.py` | 0.688s | ✅ PASS | OK signature golden matches oracle (5 fixtures, oracle_commit=46952a6846f662fdc4d6c894ef56bcfdf4a5ef85) |
| changepoint | `dump_changepoint.py` | 0.560s | ✅ PASS | OK changepoint golden matches oracle (9 scenarios, oracle_commit=da19e14d4544a60fc8d5eb9204a63815980c4bab) |
| timeline | `dump_timeline.py` | 0.466s | ✅ PASS | OK timeline golden (4 scenarios) |
| dedupe | `dump_dedupe.py` | 0.478s | ✅ PASS | OK dedupe golden (10 scenarios) |
| line_select | `dump_line_select.py` | 0.508s | ✅ PASS | OK line_select golden (16 cases) |
| pipeline | `dump_pipeline.py` | 0.531s | ✅ PASS | OK pipeline golden matches oracle (8 scenarios, oracle_commit=e58f18ca2e6877a3f7cf071bdd9f74f76ac6f67d) |
| extractor | `dump_extractor.py` | 0.522s | ✅ PASS | OK extractor golden matches oracle (/Volumes/lab/pp/SubLift/benchmark/parity/goldens/extractor/extractor.v1.json) |
| vision | `dump_vision.py` | 0.446s | ✅ PASS | [OK] Vision golden check passed: /Volumes/lab/pp/SubLift/benchmark/parity/goldens/vision/vision.v1.json |
| paddle | `dump_paddle.py` | 0.411s | ✅ PASS | [OK] Golden file /Volumes/lab/pp/SubLift/benchmark/parity/goldens/paddle/paddle.v1.json matches generated output |
| ipc_session | `dump_ipc_session.py` | 0.326s | ✅ PASS | [OK] IPC session sequence matches golden perfectly. |

## 2. 运行时门禁 (Runtime Benchmark Gate)

| 指标项 | Python Worker | C++ Worker | 比值 (C++/Py) | 断言规则 | 结论 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Wall-time Duration** | 0.109s | 0.122s | 1.12x | $T_{cpp} \le T_{py} \times 1.30 + 0.05\text{s}$ | ✅ PASS |
| **Cancel Latency** | 36.4ms | 5.6ms | 0.15x | $L_{cancel} \le 1.0\text{s}$ | ✅ PASS |
| **Restart Latency** | 2.8ms | 3.0ms | 1.07x | $L_{restart} \le 5.0\text{s}$ | ✅ PASS |
| **Peak RSS Memory** | 110.1 MiB | 14.8 MiB | 0.13x | $RSS_{cpp} \le RSS_{py} \times 1.50$ | ✅ PASS |

> Wall 硬门为 ×1.30+0.05s；契约告警阈 ×1.10 为记录式，不单独硬失败。

## 3. GT L3 水位门禁 (Fixed-asset Quality Gate)

- **状态**：✅ MEASURED
- **说明**：timing_f1=0.9767(ok), timing_precision=0.9882(ok), usable_subtitle_recall=0.9195(ok), cer_macro=0.0320(ok), text_noise=0.0000(ok), text_empty=0.0000(ok)
- **耗时**：10.6s

| 指标 | 实测 | 冻结水位 | 方向 | 结论 |
| :--- | ---: | ---: | :--- | :--- |
| timing_f1 | 0.9767 | ≥ 0.9520 | higher | ✅ PASS |
| timing_precision | 0.9882 | ≥ 0.9880 | higher | ✅ PASS |
| usable_subtitle_recall | 0.9195 | ≥ 0.8510 | higher | ✅ PASS |
| cer_macro | 0.0320 | ≤ 0.0660 | lower | ✅ PASS |
| text_noise | 0.0000 | ≤ 2.0000 | lower | ✅ PASS |
| text_empty | 0.0000 | ≤ 1.0000 | lower | ✅ PASS |

## 4. Cutover 结论

Cutover 门禁判定：**✅ PASS**。正确性 Parity (11 goldens) 通过; 运行时 Wall/Cancel/Restart/RSS 通过; GT L3 实测通过冻结水位。

已执行门禁均满足本脚本硬阈值（含 restart≤5s / cancel≤1s）。