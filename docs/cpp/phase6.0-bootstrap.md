# Phase 6.0 — Foundation / Bootstrap

> 子阶段编码：`S = 0` → feature 前缀 `feat-060xx`  
> 任务跟踪：`docs/phases/phase6.json`  
> 总览：`phase6-overview.md`  
> **进 6.1 门槛：** `feat-06001`–`feat-06005` 全部 done

## 1. 目标

建立可编译测试的 C++ 地基，并**在写算法前**冻结：target 图、图像/Config、Oracle/parity、Worker/IPC、引擎矩阵与 cutover/回滚。  
**本子阶段结束时，产品默认路径仍为 Python。**

## 2. 做 / 不做

### 做

1. 迁移与契约文档（`docs/cpp/`）+ 主文档发现性回链。
2. CMake multi-target 壳 + Catch2 + nlohmann 策略锁定。
3. `ImageBuffer`/`ImageView`、强类型 box、**完整 Config**、models。
4. 冻结 Oracle 的 parity harness 骨架（含 config golden）。
5. Worker/IPC + 引擎/cutover 设计契约（实现可在 6.5/6.6）。

### 不做

- 不实现 signature 及之后算法（6.1+）。
- 不切换 GUI/CLI 默认 runtime。
- 不上 libav、不实现原生 Paddle。

## 3. 启动任务一览

| ID | 名称 | 目标（验收一句话） |
|---|---|---|
| **feat-06001** | 文档与跟踪 + 编号 | docs/cpp 起步、phase6 可跟踪 |
| **feat-06002** | C++ 工程骨架 + **target 图** | multi-target CMake、Catch2、JSON 库策略、smoke 绿 |
| **feat-06003** | models / **完整 Config** / 图像契约 | ImageBuffer 所有权、强类型 box、Config 全字段单测 |
| **feat-06004** | Parity harness + **冻结 Oracle** | parity-contract 可执行；config golden + oracle_commit |
| **feat-06005** | **设计冻结**：IPC / 引擎矩阵 / cutover | worker-ipc + engine-matrix 文档验收；fixture 格式；无产品代码强制 |

依赖：

```text
feat-06001
    └── feat-06002
            └── feat-06003
                    └── feat-06004
                            └── feat-06005   # 文档为主；可与 06004 部分并行，但 done 须在 6.1 前
```

> 实务：`feat-06005` 以文档为主，可在 06002 之后提前起草；**状态 done 的硬依赖**为契约审阅完成且索引进 README/overview。实现代码不阻塞 06005 done。

---

## 4. 任务详述

### feat-06001 — 文档与跟踪体系 + 编号规则

（已完成）起步索引、NAMING、overview、bootstrap、phase6.json、feature-list、ADR-0020。

---

### feat-06002 — C++ 工程骨架 + target 图

**相对初版加严：**

| 必须 | 说明 |
|---|---|
| Target 壳 | `sublift_core`、`sublift_ffmpeg`、`sublift_worker`、`sublift_cli`、`sublift_test_support` 至少声明；vision 用 option 关闭 |
| 依赖方向 | 符合 [architecture.md](architecture.md) §2；core 无 ObjC |
| 测试框架 | **Catch2 v3** 锁定 |
| JSON | **nlohmann/json**（FetchContent）策略写入 cpp/README |
| OpenCV | 可选 find；06002 可不链 |
| Visibility / 静态库默认 | 按 architecture §2.1 |
| Smoke | ≥1 CTest 通过 |
| 文档 | target 图与构建命令 |

**不做：** 算法、真实 ffmpeg/Vision 逻辑。

---

### feat-06003 — models / 完整 Config / 图像契约

**相对初版加严：禁止「vector 占位以后再换类型」。**

| 必须 | 说明 |
|---|---|
| `ImageBuffer` / `ImageView` | 所有权、RGB24/BGR24/Gray8、stride、ROI view、只读语义见 architecture §3 |
| 公共 API | **不**以 `cv::Mat` 为模块边界类型 |
| 强类型 box | `SourceBox` / `FrameLocalBox` / `OcrCropBox`（或等价） |
| 整数宽度 | 几何 `int32_t`，`timestamp_ms` `int64_t` |
| **完整 `Config`** | 对齐 `src/sublift/config.py` 全部字段 + Signature/ChangePoint 嵌套，非子集 |
| 单测 | 默认 Config 字段级快照；SCRIPT_*；ImageView ROI 不拷贝语义 |

---

### feat-06004 — Parity harness + 冻结 Oracle

**相对初版加严：Oracle ≠ 裸 main。**

| 必须 | 说明 |
|---|---|
| [parity-contract.md](parity-contract.md) | schema v1；L0–L4；epsilon；bankers_round；Unicode NFC |
| golden 中间量清单 | signature/events/segments/ocr_decisions… 写入契约（6.1 填数据） |
| 可运行 | dump 完整默认 Config + metadata（`oracle_commit` 等）+ C++ compare |
| 扩展指南 | 如何加 signature golden |

---

### feat-06005 — Worker/IPC、引擎矩阵、Cutover/回滚设计冻结

**纯设计验收（可含空夹具目录），不实现 C++ worker。**

| 必须 | 说明 |
|---|---|
| [worker-ipc-contract.md](worker-ipc-contract.md) | 时序、path/frame 状态机、所有权、cancel、video_id、capability |
| [engine-matrix-and-cutover.md](engine-matrix-and-cutover.md) | vision/mock→C++；paddle→Python；禁止静默 fallback；CLI 形态；回滚 |
| Cutover 门列表 | 质量 + 运行时 + ASan + 双轨开关（文档级） |
| Fixture 说明 | 至少 1 个期望消息序夹具格式（可先 Python-only） |
| 主文档回链 | ARCHITECTURE / REQUIREMENTS / README 能发现 Phase 6 |

**验收：** 评审确认无未决冲突（尤其「全面 cutover」vs「无 Paddle C++」）；`jq`/链接检查。

---

## 5. 6.0 完成定义

- [ ] `feat-06001`–`feat-06005` 全部 done + evidence
- [ ] architecture / parity / worker-ipc / engine-matrix 四契约在 `docs/cpp/README` 可索引
- [ ] `cpp/` 可构建（有 CMake 时）；Python `./init.sh` 仍绿
- [ ] 产品默认仍 Python
- [ ] 下一刀：`feat-06101` signature parity（新开 6.1 文档与任务）

## 6. 风险

| 风险 | 缓解 |
|---|---|
| 未冻 Oracle 就比 SRT | 06004 门禁 |
| Image 类型中途更换 | 06003 禁止占位 API |
| Worker「同协议」误解 | 06005 时序契约 |
| Paddle 与 cutover 冲突 | 引擎矩阵双 worker |
| 部署假设推翻 CMake | architecture 部署摘要 + 6.7 |

## 7. 附录：审查采纳摘要（P1）

来源：进 6.1 前契约审查。已落入：

1. 冻结 Oracle（非漂移 main）→ parity-contract  
2. ImageBuffer 与完整 Config → architecture + 06003  
3. CMake target 图与工具链 → architecture + 06002  
4. Worker 时序/所有权 → worker-ipc-contract + 06005  
5. Paddle/cutover 矩阵 → engine-matrix + 06005  
6. Cutover 运行时与回滚门 → engine-matrix + 06005  
