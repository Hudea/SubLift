# Native Runtime 契约

## 1. 产品入口

| 入口 | 默认执行路径 |
|---|---|
| macOS Workbench / Task Center | Swift → UDS → `sublift_worker` |
| Native CLI | CLI → UDS → `sublift_worker` |
| Web Workbench / Task Center | Browser → HTTP/SSE → `sublift_server` → 进程内 Application/Pipeline |
| Python Oracle | 仅 macOS/CLI 显式指定 `runtime=python` 或 `SUBLIFT_RUNTIME=python` |

Web Server 是纯 Native 宿主，不提供隐式 Python 路由。

## 2. 引擎与 capability

| 引擎 | Native 条件 | 不满足条件时 |
|---|---|---|
| `vision` | macOS 构建包含可用 Vision Adapter | 明确不可用 |
| `paddle` | 构建包含 Paddle Adapter，ORT 与模型资源可解析 | 明确不可用 |
| `mock` | Worker 显式提供开发/测试 capability | 明确不可用 |

宿主只能展示目标进程实际声明的引擎。引擎选择发生在宿主装配阶段，Pipeline 不选择引擎。

## 3. Fail-closed

- 默认 runtime 是 C++；Native capability 缺失时不得自动进入 Python。
- 请求的引擎不可用、未知或与 Worker 绑定引擎不一致时，任务必须失败。
- 禁止把 Paddle 请求静默改为 Vision/Mock，或把 Vision 请求静默改为 Paddle。
- Python 只在用户或开发者显式指定时作为 Oracle、benchmark 或回滚路径。
- `SUBLIFT_ENABLE_OPENCV=OFF` 构建只可作为诊断 Worker；不得宣告产品引擎或接受正常任务。
- runtime、引擎、模型与 capability 身份必须出现在握手、日志或系统信息中，不得伪装成功。

## 4. 配置与资源

- CLI、Swift 与 Web 请求先在宿主边界解析和校验，再映射为 Native Config。
- 每个 job 使用配置快照；运行中不读取 UI 的后续设置变化。
- `ResourceLocator` / `ModelBundle` 负责运行时资源解析，Core/Pipeline 不查找安装路径。
- 模型、ORT、FFmpeg 或媒体路径失败必须返回可操作错误，不得改变请求语义。
- 正式分发是否包含模型、动态库、签名和公证由发布范围单独定义，不能从开发构建 capability 推断。

Worker 的传输与消息时序见 [Worker IPC 契约](worker-ipc-contract.md)。
