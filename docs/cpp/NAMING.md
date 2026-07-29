# Phase 6+ Feature 编号与命名规则

> 自 Phase 5 起引入「阶段 + 子阶段 + feature」编码。  
> Phase 6 起将规则写死为本文件；Phase 5 的 `feat-05001` 等历史 ID **不改写**。

## 1. Feature ID 格式

```text
feat-<PP><S><FF>
```

| 段 | 位数 | 含义 | 示例 |
|---|:---:|---|---|
| `PP` | 2 | **阶段（Phase）** | `06` = Phase 6 |
| `S` | 1 | **子阶段（minor）** | `0` = 6.0，`1` = 6.1，… |
| `FF` | 2 | **feature 序号**（子阶段内从 01 起） | `01`、`02`、… |

### 示例

| ID | 解读 |
|---|---|
| `feat-06001` | Phase **6**，子阶段 **6.0**，第 **01** 个 feature |
| `feat-06002` | Phase 6，6.0，第 02 个 feature |
| `feat-06101` | Phase 6，子阶段 **6.1**，第 01 个 feature |
| `feat-05001` | Phase 5，5.0，第 01 个 feature（历史，三位写法在文档中曾写作 `001`，**字符串同形**） |

### 与 Phase 5 文档表述的关系

- Phase 5 ADR / plan 曾写作：`05` + `0` + `001` → `feat-05001`。
- 有效字符串始终是 **5 位数字体**（`05001` / `06001`）。
- **Phase 6 起规范表述**为：`PP`(2) + `S`(1) + `FF`(2)。  
  子阶段内 feature 序号空间为 `01`–`99`，足够使用；不引入第三位。

Phase 1–4 旧编号 `feat-001` ~ `feat-043` **保持不动**，两套规则并存。

## 2. 子阶段命名

| 写法 | 用途 |
|---|---|
| `6.0` / `phase6.0` | 人类可读子阶段 |
| `S = 0` | 编码中的子阶段位 |
| `docs/cpp/phase6.0-*.md` | 该子阶段设计文档文件名 |
| `phase6.foundation` 等 | `feature-list.json` 中的**项目级功能块** id（点分，非 feat id） |

子阶段主题约定（Phase 6）：

| 子阶段 | 主题（概要） | feature 前缀 |
|---|---|---|
| **6.0** | Foundation：文档、CMake targets、models/完整 Config、parity Oracle、IPC/引擎/cutover 契约 | `feat-060xx` |
| **6.1** | Pure pipeline 算法 parity（signature → … → line_select） | `feat-061xx` |
| **6.2** | C++ Pipeline 流式编排 | `feat-062xx` |
| **6.3** | FFmpeg extractor 行为复刻 | `feat-063xx` |
| **6.4** | Vision Objective-C++ adapter | `feat-064xx` |
| **6.5** | C++ worker（UDS + JSON 协议兼容） | `feat-065xx` |
| **6.6** | Cutover（默认路径切换 + benchmark 门） | `feat-066xx` |
| **6.7** | PaddleOCR C++ Native MVP（ONNX / PP-OCRv6） | `feat-067xx` |
| **6.8** | Paddle Native 质量 / 性能加固与产品 cutover | `feat-068xx` |
| **6.9+** | 去 Python 产品 runtime / 打包分发等（后置） | `feat-069xx`… |

> 子阶段划分以 `docs/cpp/phase6-overview.md` 为准；上表为路线图，可在不改 `PP`/`S` 语义的前提下微调范围。

## 3. 分支与提交

| 类型 | 约定 |
|---|---|
| 子阶段起步分支 | `feat/cpp-6.0-bootstrap`、`feat/cpp-6.1-signature` 等 |
| 提交前缀 | `feat(phase6.0):` / `docs(phase6):` / `test(phase6.0):` |
| 引用任务 | 正文或 footer 写 `feat-06001` |

## 4. 文档与跟踪文件职责

| 产物 | 放哪里 | 编号是否出现 |
|---|---|---|
| 迁移计划 / 设计 | **`docs/cpp/`** | 可引用 feat id |
| Phase 任务 + evidence | `docs/phases/phase6.json` | **必须**使用 `feat-06SFF` |
| 项目级大功能块 | `feature-list.json` → `phases[phase6]` | `covers: ["feat-06001", …]` |
| 会话导航 | `progress.md` | 可写当前 feat id |
| 长期决策 | `docs/DECISIONS.md` | ADR 可引用 |

## 5. 禁止

- 不要用 `feat-6-1`、`feat-6001`、`CPP-001` 等非规范 id。
- 不要跳过子阶段位直接把 6.1 的任务编成 `feat-060xx`。
- 不要在同一 PR 混多个子阶段的「完成声明」（实现可预研，验收按 feat 独立）。
