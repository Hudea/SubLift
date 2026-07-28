# C++ 迁移文档（`docs/cpp/`）

本目录是 **Phase 6 — Native C++ Core Migration** 的计划与设计文档落点。  
**任务状态与验证证据** 仍以 `docs/phases/phase6.json` 为唯一事实来源；项目级功能块见根目录 `feature-list.json`。

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
3. **SwiftUI + UDS 保留**；Worker 换 C++（vision/mock）；paddle 见引擎矩阵。
4. **`sublift_core` 不依赖 ObjC/Swift/Vision**；target 依赖图见 architecture。
5. **一次只做一个可独立验收 feature**（`AGENTS.md`）。
6. **6.3 done**：FFmpeg Extractor parity（含 phase-review 加固）；产品路径仍 Python。下一子阶段 **6.4 Vision**。
