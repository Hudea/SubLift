# Phase 6.8 C++ Paddle 设计、修改与验收回顾索引

> - **状态：** Phase 6.8 已完成并通过最终验收
> - **范围：** `feat-06801`–`feat-06807`
> - **用途：** 回顾“为什么做、如何设计、具体改了什么、如何证明、还剩什么”
> - **证据事实源：** `docs/phases/phase6.json`；本页只负责导航，不复制或取代
>   feature evidence

## 1. 推荐阅读路径

### 10 分钟结论回顾

1. [最初质量审计](../reports/phase6.7-paddle-quality-audit-2026-07-29.md)：
   为什么 Phase 6.7 的简化 Det 与 `2.74x` 性能不能接受。
2. [Phase 6.8 主设计](phase6.8-paddle-hardening.md)：
   目标、非目标、依赖顺序、不变量、7 个 feature 和完成定义。
3. [最终验收报告](../reports/phase6.8-paddle-cutover.md)：
   算子、质量、性能、默认路由、长流、取消、重启和回滚的最终数值。

### 设计与实现深度回顾

1. [Phase 6.7 Native MVP 设计](phase6.7-paddle.md)：理解改造前基线与 adapter
   选型。
2. [Phase 6.8 主设计](phase6.8-paddle-hardening.md)：按 `06801 → 06807`
   阅读设计和完成证据。
3. [架构边界](architecture.md)：确认 `sublift_core`、`sublift_paddle`、
   Worker 和平台 adapter 的依赖方向。
4. [Parity 契约](parity-contract.md)：理解冻结 Python Oracle、中间量 golden
   与误差门。
5. [引擎矩阵与回滚](engine-matrix-and-cutover.md)：理解当前产品默认、fallback
   与一键回滚。
6. [ADR-0023–0029](../DECISIONS.md#adr-0029-paddle-全门通过后默认-cbuild-tree-自带已验收-ort2026-07-30)：
   回顾关键取舍及被后续决策替代的历史策略。
7. [关键障碍与解决记录](../HURDLES.md)：回顾 unclip 1px 放大、ORT 二进制归因和
   batch=1 优化被拒绝的排查过程。

### 逐项验收复跑

1. [专项门使用说明](../../scripts/parity/README.md#paddle-stage-golden-feat-06802)
2. [Feature 状态与完整 evidence](../phases/phase6.json)
3. [最终 cutover gate 实现](../../scripts/parity/check_paddle_cutover.py)
4. [最终验收报告](../reports/phase6.8-paddle-cutover.md)

## 2. 从问题到修复的修改记录

| Feature | 修改目的 | 主要实现落点 | 设计 / 决策 | 验证入口 |
|---|---|---|---|---|
| `feat-06801` | hardening 期间默认回 Python；C++ 仅显式 experimental，禁止换成 Vision/Mock | [Python runtime policy](../../src/sublift/runtime.py)、[Swift runtime policy](../../apps/macos/Sources/SubLiftMac/Core/RuntimePolicy.swift)、CLI/Worker 状态标识 | [6.8 §06801](phase6.8-paddle-hardening.md#feat-06801--安全路由与-experimental-标识)、[ADR-0024](../DECISIONS.md#adr-0024-phase-68-paddle-native-质量--性能加固先于去-python-分发2026-07-29) | Python/Swift 路由矩阵 |
| `feat-06802` | 建立 Det/Cls/Rec 十阶段可观测性，冻结模型、参数、二进制和 fixtures | [stage dump](../../scripts/parity/dump_paddle_stages.py)、[trace driver](../../scripts/parity/paddle_stage_trace.py)、[frozen manifest](../../scripts/parity/freeze_paddle_manifest.json)、[v2 golden](../../benchmark/parity/goldens/paddle/paddle_stages.v2.json) | [6.8 §06802](phase6.8-paddle-hardening.md#feat-06802--分阶段观测与冻结-oracle-fixture) | [stage golden](../../scripts/parity/README.md#paddle-stage-golden-feat-06802) |
| `feat-06803` | 用完整 DB/dilation/contour/score/unclip 替换连通域 AABB；统一 Det 预处理与空结果 | [Det preprocess](../../cpp/src/paddle/ppocr_det_preprocess.cpp)、[DB postprocess](../../cpp/src/paddle/ppocr_db_postprocess.cpp)、[Det tests](../../cpp/tests/paddle_det_preprocess_test.cpp) | [6.8 §06803](phase6.8-paddle-hardening.md#feat-06803--det-预处理与完整-dbunclip-parity)、[ADR-0025](../DECISIONS.md#adr-0025-paddle-数值门必须区分-ort-版本与二进制构建2026-07-30) | [Det live gate](../../scripts/parity/check_paddle_det_parity.py) |
| `feat-06804` | 对齐 quad perspective crop、180° Cls、动态 Rec batch/padding、模型字典和 CTC | [crop/Cls](../../cpp/src/paddle/ppocr_crop_cls.cpp)、[models](../../cpp/src/paddle/paddle_models.cpp)、[OCR orchestration](../../cpp/src/paddle/paddle_ocr.cpp)、[CTC](../../cpp/src/paddle/ppocr_ctc.hpp) | [6.8 §06804](phase6.8-paddle-hardening.md#feat-06804--quad-cropcls-与-rec-parity)、[ADR-0026](../DECISIONS.md#adr-0026-paddle-rec-字典与-uint8-resize-必须具有模型算术级确定性2026-07-30) | [Crop/Cls/Rec gate](../../scripts/parity/check_paddle_rec_parity.py) |
| `feat-06805` | 建立真实双 runtime、多来源、fail-closed 的 E2E 质量门 | [quality gate](../../scripts/parity/check_paddle_gate.py)、[quality manifest](../../benchmark/datasets/paddle_quality/manifest.v1.json)、[frozen baseline](../../benchmark/parity/goldens/paddle/paddle_quality_baseline.v1.json) | [6.8 §06805](phase6.8-paddle-hardening.md#feat-06805--多源-paddle-e2e-质量门)、[ADR-0027](../DECISIONS.md#adr-0027-paddle-e2e-门必须真实运行det-几何漂移不得靠-cls-阈值掩盖2026-07-30) | [E2E quality gate](../../scripts/parity/README.md#paddle-e2e-quality-gate-feat-06805) |
| `feat-06806` | 在质量冻结后优化 ORT 线程、归一化与生命周期；拒绝造成输出漂移的微优化 | [Paddle models/options](../../cpp/src/paddle/paddle_models.cpp)、[performance gate](../../scripts/parity/check_paddle_perf.py)、[performance manifest](../../benchmark/datasets/paddle_performance/manifest.v1.json) | [6.8 §06806](phase6.8-paddle-hardening.md#feat-06806--paddle-native-性能加固)、[ADR-0028](../DECISIONS.md#adr-0028-paddle-性能验收固定-ort-二进制不以输出漂移换取微基准收益2026-07-30) | [canonical performance gate](../../scripts/parity/README.md#paddle-canonical-performance-gate-feat-06806) |
| `feat-06807` | 将 Paddle available 的默认切到 C++ stable；打包已验收 ORT；完成长流、取消、重启与回滚 | [CMake/ORT bundle](../../cpp/CMakeLists.txt)、[Python policy](../../src/sublift/runtime.py)、[Swift probe/policy](../../apps/macos/Sources/SubLiftMac/Core/RuntimePolicy.swift)、[GUI runtime log](../../apps/macos/Sources/SubLiftMac/Core/SubtitleExtractor.swift)、[cutover gate](../../scripts/parity/check_paddle_cutover.py) | [6.8 §06807](phase6.8-paddle-hardening.md#feat-06807--paddle-c-产品-cutover)、[ADR-0029](../DECISIONS.md#adr-0029-paddle-全门通过后默认-cbuild-tree-自带已验收-ort2026-07-30) | [product cutover gate](../../scripts/parity/README.md#paddle-product-cutover-gate-feat-06807) |

逐 feature 的完整 `description`、`subtasks`、依赖、状态和实际命令/数值记录在
[phase6.json](../phases/phase6.json) 中。它是修改完成证据的唯一事实源；上表用于从
设计快速跳转到实现与验证。

## 3. 关键架构与算子记录

| 回顾问题 | 权威记录 |
|---|---|
| 为什么不直接调用 RapidOCR C++ SDK？ | [Phase 6.7 选型](phase6.7-paddle.md)、[ADR-0023](../DECISIONS.md#adr-0023-phase-67-paddleocr-c-adapter-选型与路由2026-07-29) |
| 为什么 `sublift_core` 不能依赖 ORT/OpenCV/Paddle？ | [Phase 6.8 全局不变量](phase6.8-paddle-hardening.md#4-全局不变量)、[C++ architecture](architecture.md) |
| Det 如何从简化 AABB 升级为完整 DB/unclip？ | [6.8 §06803](phase6.8-paddle-hardening.md#feat-06803--det-预处理与完整-dbunclip-parity)、[ADR-0025](../DECISIONS.md#adr-0025-paddle-数值门必须区分-ort-版本与二进制构建2026-07-30) |
| Quad/Cls/Rec/CTC 如何与 Python 对齐？ | [6.8 §06804](phase6.8-paddle-hardening.md#feat-06804--quad-cropcls-与-rec-parity)、[ADR-0026](../DECISIONS.md#adr-0026-paddle-rec-字典与-uint8-resize-必须具有模型算术级确定性2026-07-30) |
| 为什么 ORT 版本相同仍要固定 dylib SHA？ | [ADR-0025](../DECISIONS.md#adr-0025-paddle-数值门必须区分-ort-版本与二进制构建2026-07-30)、[ADR-0028](../DECISIONS.md#adr-0028-paddle-性能验收固定-ort-二进制不以输出漂移换取微基准收益2026-07-30) |
| 为什么拒绝 Rec batch=1？ | [ADR-0028](../DECISIONS.md#adr-0028-paddle-性能验收固定-ort-二进制不以输出漂移换取微基准收益2026-07-30)：微基准更快但改变 Latin SRT SHA |
| 当前 runtime/fallback/rollback 到底是什么？ | [终局引擎矩阵](engine-matrix-and-cutover.md#1-引擎--runtime-矩阵终局策略)、[ADR-0029](../DECISIONS.md#adr-0029-paddle-全门通过后默认-cbuild-tree-自带已验收-ort2026-07-30) |
| 为什么 build tree 要自带 ORT？ | [ADR-0029](../DECISIONS.md#adr-0029-paddle-全门通过后默认-cbuild-tree-自带已验收-ort2026-07-30)、[最终报告](../reports/phase6.8-paddle-cutover.md#构建可复现性与剩余边界) |
| 哪些尝试失败、如何定位根因？ | [HURDLES](../HURDLES.md)：1px unclip→Cls 误旋转、同版本 ORT 构建差异、batch=1 输出漂移 |

## 4. 验收证据地图

| 验收维度 | 最终结果 | 记录 |
|---|---|---|
| Det | tensor/probability/quad exact；9/9 box P/R=1.0 | [06803 evidence](../phases/phase6.json)、[Det gate](../../scripts/parity/check_paddle_det_parity.py) |
| Crop/Cls/Rec/CTC | 固定 quad 后 tensor、token、text、order exact | [06804 evidence](../phases/phase6.json)、[Rec gate](../../scripts/parity/check_paddle_rec_parity.py) |
| E2E 质量 | 3 来源、614.272s，逐源 SRT SHA exact，全部指标 delta=0 | [06805 evidence](../phases/phase6.json)、[quality gate](../../scripts/parity/check_paddle_gate.py) |
| 性能 | C++/Python wall `0.8956x`；RSS `0.9152x` | [06806 evidence](../phases/phase6.json)、[ADR-0028](../DECISIONS.md#adr-0028-paddle-性能验收固定-ort-二进制不以输出漂移换取微基准收益2026-07-30) |
| 产品切换 | 默认 C++ → 强制 Python → 默认 C++，三次 SRT SHA exact | [最终验收报告](../reports/phase6.8-paddle-cutover.md#产品路由与回滚) |
| 长流 / 生命周期 | 720s；cancel 2.8ms；restart readiness 0.1ms | [最终验收报告](../reports/phase6.8-paddle-cutover.md#长流与生命周期) |
| 工程门 | init、Paddle C++、Python、Swift 全过 | [最终验收报告](../reports/phase6.8-paddle-cutover.md#工程验证) |

## 5. 当前边界与历史文档阅读规则

- [Phase 6.7 质量审计](../reports/phase6.7-paddle-quality-audit-2026-07-29.md)
  和 Phase 6.8 主设计开头的 `2.74x`、简化 Det、experimental 路由是**修复前历史状态**。
- 当前产品状态以 [引擎矩阵](engine-matrix-and-cutover.md)、
  [ADR-0029](../DECISIONS.md#adr-0029-paddle-全门通过后默认-cbuild-tree-自带已验收-ort2026-07-30)
  和 [最终验收报告](../reports/phase6.8-paddle-cutover.md)为准。
- Q2 的 3 个来源中只有 1 个外置真实片源，另外 2 个是确定生成源；这足以冻结本轮
  相对门，但不是广泛真实世界泛化承诺。
- Release 全量 CTest 的 174/175 中，唯一失败是既有 Apple Vision synthetic OCR
  环境用例；标准 Debug `./init.sh` 与 Paddle Release 专项门均通过。
- 正式 `.app` 的模型/ORT/ffmpeg 随包、universal2、签名与公证属于 6.9+，不属于
  Phase 6.8 的“开发 build 自带 ORT”。
