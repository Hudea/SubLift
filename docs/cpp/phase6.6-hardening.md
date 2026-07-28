# Phase 6.6 — C++ Worker Hardening（feat-06606）

> 状态：**done（标准开发门）**；live GT L3 与 macOS sanitizer 仍是 release gate。
> 依据：[迁移质量审查报告](../reports/phase6-cpp-migration-quality-audit-2026-07-28.md)
> 任务事实源：`docs/phases/phase6.json`；本文件是实现计划，不替代验证证据。

## 目标与边界

在不改变 C++ Core、UDS 边界和 vision/mock → C++、paddle → Python 路由的前提下，收口默认 C++ Worker 的验证与协议可靠性。完成后，标准启动路径必须全绿，连接关闭、能力宣告及 legacy frame 输入均与契约一致。

不纳入本 feature：原生 Paddle、libav、OCR/打轴算法调整、GT L3 资产或发布级 sanitizer CI；后二者保留为 release gate。

## 工作分解与验收

| 顺序 | 子任务 | 实现落点 | 验收 |
|---|---|---|---|
| 1 | 可移植 E2E 视频夹具 | `tests/ipc/test_cpp_worker.py` | 不依赖 ffmpeg `drawtext`；检查 ffmpeg 退出码与产物；path/cancel 测试稳定通过。 |
| 2 | 生命周期与 capability | `connection.cpp`、`engine_factory.cpp`、IPC 测试 | 客户端 `bye` 后 Worker 清理并 EOF；握手仅宣告当前绑定引擎可接受的 capability。 |
| 3 | frame-mode 输入防护 | `bridge.cpp`、`protocol.cpp`、测试 | 限制 base64/JPEG 字节、JPEG 签名、像素数和溢出；拒绝非法 fps 与几何。 |
| 4 | 默认验证与文档对齐 | `init.sh`、`ARCHITECTURE.md`、IPC contract | C++ 默认路径的工具链不再静默跳过；`mypy src tests`；架构状态与 runtime matrix 一致。 |
| 5 | 收口验证 | pytest、ruff、mypy、CTest、Swift、cutover | 全量 Python/C++/Swift 测试通过；报告只将 GT L3 waiver 与 sanitizer 作为剩余 release 风险。 |

## 架构决定

本轮不引入新的 `JobSession` 类型。现有 `WorkerConnection → BridgeHandler` 边界足以以最小风险修复协议与资源约束；待真实并发/多 job 需求出现时，再以独立 feature 将 BridgeHandler 重构为显式状态机。此处新增测试先锁定关闭、取消和输入边界，避免在重构前继续积累行为漂移。
