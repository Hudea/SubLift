# Native Runtime 契约

> ADR-0038 已确认 Native-only 目标。D02 已关闭产品宿主上的双 runtime：Native CLI、macOS
> 与 Web 只启动 Native Worker/Server；`--runtime` 与 `SUBLIFT_RUNTIME=python` 不再选择
> 实现。产品路径已无 Python CLI/IPC。隔离 Oracle 与 benchmark 属于离线工具，不进入产品运行。

## 1. 产品入口

| 入口 | 唯一执行路径 |
|---|---|
| macOS Workbench / Task Center | Swift → UDS → `sublift_worker` |
| Native CLI | CLI → UDS → `sublift_worker` |
| Web Workbench / Task Center | Browser → HTTP/SSE → `sublift_server` → 进程内 Application/Pipeline |

产品不接受 runtime 选择；C++ 是唯一 runtime。任何宿主都不得 import、启动或探测 Python
Pipeline/IPC，也不得把离线工具作为产品依赖。

## 2. 引擎与 capability

| 引擎 | Native 条件 | 不满足条件时 |
|---|---|---|
| `vision` | macOS 构建包含可用 Vision Adapter | 明确不可用 |
| `paddle` | 构建包含 Paddle Adapter，ORT 与模型资源可解析 | 明确不可用 |
| `mock` | Worker 显式提供开发/测试 capability | 明确不可用 |

宿主只能展示目标进程实际声明的引擎。引擎选择发生在宿主装配阶段，Pipeline 不选择引擎。

## 3. Fail-closed 与回滚

- Native capability 缺失时不得进入 Python、切换引擎或返回伪成功。
- 请求的引擎不可用、未知或与 Worker 绑定引擎不一致时，任务必须失败。
- 禁止把 Paddle 请求静默改为 Vision/Mock，或把 Vision 请求静默改为 Paddle。
- `SUBLIFT_ENABLE_OPENCV=OFF` 构建只可作为诊断 Worker；不得宣告产品引擎或接受正常任务。
- runtime、引擎、模型与 capability 身份必须出现在握手、日志或系统信息中，不得伪装成功。
- 产品回滚以此前已验收的 Native artifact、release/tag 或 Git revision 为单位；不得在同一
  产品版本内切换到 Python runtime。

## 4. 配置与资源

- CLI、Swift 与 Web 请求先在宿主边界解析和校验，再映射为 Native Config。
- 每个 job 使用配置快照；运行中不读取 UI 的后续设置变化。
- `ResourceLocator` / `ModelBundle` 负责运行时资源解析，Core/Pipeline 不查找安装路径。
- 模型、ORT、FFmpeg 或媒体路径失败必须返回可操作错误，不得改变请求语义。
- 产品资源准备和构建不得要求 Python、`.venv`、Python wheel 或 RapidOCR cache；固定
  manifest/SHA 是 Native artifact 的一部分。
- 正式分发是否包含模型、动态库、签名和公证由发布范围单独定义，不能从开发构建 capability 推断。

## 5. 隔离离线工具

产品路径已关闭 Python CLI、IPC 与 runtime 选择。剩余 Python 只作为隔离离线工具：

- 公开命名空间：`sublift_offline` / `sublift-benchmark`；不占用产品 `sublift` 命令。
- 冻结 Oracle 位于 `src/sublift/` 算法副本，仅复现历史 golden 或专项对照。
- 离线验证入口：`scripts/verify-offline.sh`。产品日常验证：`scripts/verify-product.sh`。

可选 Python 工具不得成为任何产品门的唯一执行器或行为权威。

Worker 的传输与消息时序见 [Worker IPC 契约](worker-ipc-contract.md)。
