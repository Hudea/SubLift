# C++ 迁移文档（`docs/cpp/`）

本目录是 **Phase 6 — Native C++ Core Migration** 的计划与设计文档落点。  
**任务状态与验证证据** 仍以 `docs/phases/phase6.json` 为唯一事实来源；当前项目 Phase 索引见根目录 `phases.json`，`feature-list.json` 仅保留历史兼容功能组。

## 文档索引

### 计划与编号

| 文件 | 用途 |
|---|---|
| [NAMING.md](NAMING.md) | feat 编号与子阶段命名规则（规范） |
| [phase6-overview.md](phase6-overview.md) | Phase 6 总览：目标、路线、非目标 |
| [phase6.0-bootstrap.md](phase6.0-bootstrap.md) | 6.0 Foundation / Bootstrap（**done**） |
| [phase6.1-pure-pipeline.md](phase6.1-pure-pipeline.md) | 6.1 Pure pipeline 算法 parity（**done**） |
| [phase6.2-pipeline.md](phase6.2-pipeline.md) | 6.2 Pipeline 流式编排（**done**） |
| [phase6.3-extractor.md](phase6.3-extractor.md) | 6.3 FFmpeg Extractor（**done**） |
| [phase6.4-vision.md](phase6.4-vision.md) | 6.4 Vision OCR（**done**） |
| [phase6.5-worker.md](phase6.5-worker.md) | 6.5 C++ Worker（**done**） |
| [phase6.6-cutover.md](phase6.6-cutover.md) | **6.6 Cutover**（**done**：默认 C++ / 门禁 / 回滚） |
| [phase6.6-hardening.md](phase6.6-hardening.md) | **6.6 Worker Hardening**（feat-06606，标准开发门已完成） |
| [phase6.6-sanitizer-isolation.md](phase6.6-sanitizer-isolation.md) | **6.6 Sanitizer 依赖隔离**（feat-06607，诊断隔离已完成；发布门仍待解决） |
| [phase6.7-paddle.md](phase6.7-paddle.md) | **6.7 PaddleOCR C++ Native MVP**（ONNX / PP-OCRv6；**done**） |
| [phase6.8-paddle-hardening.md](phase6.8-paddle-hardening.md) | **6.8 Paddle Native 质量、性能与产品 Cutover**（feat-06801–06807；**done**） |
| [phase6.8-review-index.md](phase6.8-review-index.md) | **6.8 回顾索引**：问题审计 → 设计 → 逐 feature 修改 → ADR → 验收与复跑入口 |
| [phase6.9-native-product-architecture.md](phase6.9-native-product-architecture.md) | **6.9 Native 开发架构 + 未来分发目标**：已完成边界与明确后置范围 |
| [phase6.9-implementation-plan.md](phase6.9-implementation-plan.md) | **6.9 收口计划**：开发验收与 ADR-0030 发布后置边界 |
| [phase6.9-design-gap-fix.md](phase6.9-design-gap-fix.md) | **6.9 设计差距修复**：P0 composition / ResourceLocator / fail-closed |
| [phase6.9-ports-adapters-layout.md](phase6.9-ports-adapters-layout.md) | **Ports vs Adapters 分层**：ports 仅接口；adapters 具体实现 + detector 工厂 |

### 架构与契约

| 文件 | 用途 | 关联 feat |
|---|---|---|
| [architecture.md](architecture.md) | Target 图、图像/Config/接口、工具链 | 06002 / 06003 |
| [parity-contract.md](parity-contract.md) | **冻结 Oracle**、golden 中间量、L0–L4、rounding | 06004 |
| [worker-ipc-contract.md](worker-ipc-contract.md) | UDS 时序、所有权、cancel、capability | 06005 / 065 |
| [engine-matrix-and-cutover.md](engine-matrix-and-cutover.md) | 引擎×runtime 矩阵、cutover 门、回滚 | 06005 / 066 |

### 相关但不在本目录

| 路径 | 用途 |
|---|---|
| `docs/phases/phase6.json` | 细粒度任务 + evidence |
| `docs/ARCHITECTURE.md` §Phase 6 | 主架构发现入口 |
| `docs/plans/` | 历史 Phase 1–5 设计源头 |
| `cpp/` | C++ 源码树（06002 起） |
| `docs/DECISIONS.md` | ADR-0020 / 0021… |

## 原则（摘要）

1. **冻结 Oracle**（commit + 环境 + golden schema），不是「漂移中的 main」。
2. **Python dump → C++ Candidate 比较**；先中间量与决策，再 SRT/GT。
3. **SwiftUI + UDS 保留**；Worker 换 C++（vision/mock；6.7+ paddle native）；见引擎矩阵。
4. **`sublift_core` 不依赖 ObjC/Swift/Vision/ORT**；OCR adapter 分 target；图见 architecture。
5. **一次只做一个可独立验收 feature**（`AGENTS.md`）。
6. **6.6 已完成**：vision/mock 产品默认 C++ worker；当时 paddle 仍 Python；`SUBLIFT_RUNTIME=python` 回滚；门禁报告 `docs/reports/phase6.6-cutover-gate.md`。
7. **6.7 Native MVP 已完成**：Paddle C++ adapter 能运行并接入产品，但简化 Det 与 synthetic golden 不代表真实 RapidOCR parity。
8. **6.8 已完成**：完整 Det/Cls/Rec、E2E GT、性能、720s 长流和回滚全部过门；
   Paddle available 时默认 C++ stable，Python 保留 fallback；见
   [phase6.8-paddle-hardening.md](phase6.8-paddle-hardening.md)。
9. **6.9 开发架构已收口**：CMake target、目录、Ports/Adapters、Composition Root、
   ResourceLocator 与 Native CLI 已完成；默认 fail-closed，显式 Python 保留为开发工具。
   `.app`、签名、公证和最终 artifact Python-free 门按 ADR-0030 后置。
