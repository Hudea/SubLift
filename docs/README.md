# SubLift 文档索引

> 单一入口。状态：**current**（当前权威）· **historical**（只作追溯，不约束实现）·
> **proposal**（未批准）。发现文档与代码/测试冲突时，以代码、测试和 `phases.json` 的
> `detail_file` 为准，并在本索引标注漂移。

## 当前权威

| 文档 | 内容 | 状态 |
|---|---|---|
| [ARCHITECTURE.md](ARCHITECTURE.md) | 目标分层、依赖方向、运行时拓扑与合同边界 | current |
| [REQUIREMENTS.md](REQUIREMENTS.md) | 产品范围、功能状态、非功能要求与风险 | current |
| [DEVELOPMENT.md](DEVELOPMENT.md) | 环境、构建、测试、验证入口 | current |
| [TESTMAP.md](TESTMAP.md) | 测试资产、门禁分类与运行决策树 | current |
| [cpp/README.md](cpp/README.md) | Native C++ 契约入口 | current |
| [cpp/native-architecture.md](cpp/native-architecture.md) | Target/目录所有权与依赖约束 | current |
| [cpp/worker-ipc-contract.md](cpp/worker-ipc-contract.md) | UDS framing、握手、消息与时序 | current |
| [cpp/runtime-contract.md](cpp/runtime-contract.md) | 唯一 Native runtime、capability 与回滚 | current |
| [design_ui/README.md](design_ui/README.md) | macOS/Web 交互设计入口与 parity matrix | current |
| [contracts/README.md](../contracts/README.md) | 跨语言 schema、fixtures 与语义合同 | current |
| [shared/parity.md](design_ui/shared/parity.md) | Web/macOS 能力对齐、宿主差异与 gap 登记 | current |

## 设计与提案

| 文档 | 内容 | 状态 |
|---|---|---|
| [design/pipeline.md](design/pipeline.md) | Python 时代打轴/编排设计（迁移追溯） | historical |
| [design/ocr.md](design/ocr.md) | Python 时代 OCR 设计（迁移追溯） | historical |
| [design/extractor.md](design/extractor.md) | Python 时代抽帧设计（迁移追溯） | historical |
| [design/benchmark.md](design/benchmark.md) | 隔离 benchmark 工具现状与用法 | current |
| [design/macos-gui.md](design/macos-gui.md) | macOS GUI 模块设计 | current |
| [design/performance-acceleration.md](design/performance-acceleration.md) | 性能归因、P0 负向结论与 P1/P2 候选 | current |
| [design/det-input-geometry.md](design/det-input-geometry.md) | Det 输入几何实验（736 → 像素预算） | current |
| [design/det-pixel-budget-delivery.md](design/det-pixel-budget-delivery.md) | Det 像素预算交付与独立复核 | current |
| [design/adr-det-geometry-contract.md](design/adr-det-geometry-contract.md) | ADR-0042 草案：det 几何退出逐字节 parity | proposal |
| [design/det-geometry-evidence-plan.md](design/det-geometry-evidence-plan.md) | Phase 18 D03 切换前证据计划 | proposal |
| [design/det-geometry-policy-exploration.md](design/det-geometry-policy-exploration.md) | Det 几何策略探索（0.6/0.7/short-320、2K/4K、带高） | current |
| [design/gpu-execution.md](design/gpu-execution.md) | GPU 执行后端提案（未实现） | proposal |
| [design/roi-data-path.md](design/roi-data-path.md) | Phase 4 ROI 数据通路 | historical |
| [design/path-mode-overlap.md](design/path-mode-overlap.md) | Phase 4.1 未采纳实验 | historical |
| [design/ocr-performance-attribution.md](design/ocr-performance-attribution.md) | Phase 4.2 OCR 归因 | historical |

## 历史与决策

| 文档 | 内容 | 状态 |
|---|---|---|
| [DECISIONS.md](DECISIONS.md) | ADR-0001–ADR-0041，按时间追加；仍是历史决策的权威记录 | current |
| [HURDLES.md](HURDLES.md) | 踩坑、根因与解决记录 | historical |
| [phases/](phases/) | Phase detail；仅 active/recent 进入导航，done 只读 | historical/current |
| [design_ui/evidence/](design_ui/evidence/) | Phase 8/10 实际应用截图与 fixture 证据 | historical |

## 更新规则

1. **current** 文档必须与代码/测试一致；行为变更时同 PR 更新，或显式降级为 historical。
2. **historical** 文档不重写事实；只允许断链、错别字和状态勘误。
3. 新增长篇设计先进 `design/`；Phase 的执行 evidence 用引用，不把长文塞回 Phase JSON。
4. 新增/移动文档后运行 `./scripts/verify-harness.sh`（链接与 Phase 真源检查）。
