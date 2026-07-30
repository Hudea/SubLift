# Phase 6.9 设计差距修复方案（落地）

> 依据架构审核：对照 `phase6.9-native-product-architecture.md`  
> 状态：**P0/P1 本轮已落地**（2026-07-30）；SHA/manifest/ports 全迁/§19 全门仍开放

## 目标

关闭审核 P0/P1 中阻塞「设计目标达成」的缺口，使代码与规范对齐；
跟踪状态诚实（06911 blocked，§19 未全勾）。

## 批次

| 批 | 内容 | 验收 |
|----|------|------|
| A | Swift/Python 产品矩阵一致：cpp paddle 不可用 → **抛错**，仅显式 python 回滚 | Swift/Python 测试 |
| B | ResourceLocator：`Helpers`+`MacOS`；ffmpeg PATH；resolve_bin 走 Locator | ctest resource + extractor |
| C | Path media 工厂注入；application 不再直接链/构 FFmpeg；ImageIO PRIVATE | cmake graph + worker tests |
| D | `sublift_ipc` → `sublift_worker_runtime`（保留别名） | 构建 |
| E | CLI 发现 `../Helpers/sublift_worker` | 代码审查 |
| F | Paddle 产品头去掉 geometry 强依赖；Capabilities；跟踪诚实化 | 编译 + phase 状态 |

## 非本轮（明确延期）

- 完整 model SHA/manifest 下载器
- codesign/notarization（06911 blocked）
- ports 下全部具体类物理迁出（大搬家，另开）
- nlohmann 完全从 protocol public 消失（需 DTO PIMPL，另开）
