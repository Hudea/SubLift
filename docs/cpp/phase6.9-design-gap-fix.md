# Phase 6.9 设计差距修复方案（落地）

> 依据架构审核：对照 `phase6.9-native-product-architecture.md`
> 状态：**开发架构缺口已关闭**（2026-07-30）；发布项按 ADR-0030 整体后置

## 目标

关闭审核 P0/P1 中阻塞「设计目标达成」的缺口，使代码与规范对齐；
跟踪状态区分开发完成与发布后置。

## 批次

| 批 | 内容 | 验收 |
|----|------|------|
| A | Swift/Python 产品矩阵一致：cpp paddle 不可用 → **抛错**，仅显式 python 回滚 | Swift/Python 测试 |
| B | ResourceLocator：override/env/cache/system；ffmpeg PATH；resolve_bin 走 Locator | ctest resource + extractor |
| C | Path media 工厂注入；application 不再直接链/构 FFmpeg；ImageIO PRIVATE | cmake graph + worker tests |
| D | `sublift_ipc` → `sublift_worker_runtime`，最终删除旧 alias | 构建 |
| E | CLI/Swift 开发环境发现 build tree Worker | 代码审查 |
| F | Paddle 产品头去掉 geometry 强依赖；Capabilities；跟踪诚实化 | 编译 + phase 状态 |

## 发布阶段（明确后置）

- 完整 model SHA/manifest 下载器与原子安装
- `.app`、依赖随包、codesign/notarization/Gatekeeper
- 最终 artifact Python-free 干净机门

## 本次补齐

- `ports/` 删除全部具体 Adapter re-export，仅保留抽象接口；
- protocol/framing header 进入 `include/sublift/protocol/`；
- nlohmann/json 只作为 `sublift_protocol` PRIVATE 实现依赖；
- 删除 `sublift_ipc` 兼容 alias；
- 删除未达到发布门的 bundle/Python-free 脚手架。
