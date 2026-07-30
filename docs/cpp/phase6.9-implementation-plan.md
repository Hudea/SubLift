# Phase 6.9 — Native 开发架构收口

> 状态：开发范围已完成并通过最终验证
> 范围决策：ADR-0030
> 任务跟踪：`docs/phases/phase6.json`

## 1. Goal

在不修改 OCR、打轴、去重数值语义的前提下，把 6.8 已切换的 C++ 产品执行路径整理为：

- 清晰的 `core / pipeline / protocol / application / worker` Target 方向；
- `ports/` 只包含抽象接口，具体实现位于 `adapters/`；
- Worker 是唯一 Composition Root；
- Native CLI 是薄 IPC 客户端，不链接 ORT/OpenCV/Vision；
- `ResourceLocator`、capability probe 与真实 job 使用同一开发期资源解析路径；
- Paddle 不可用时默认 fail-closed，只保留显式 Python Oracle/开发回滚；
- 冻结 golden、GT、runtime、cancel/restart/RSS 门不退化。

Phase 6.9 不再承担 `.app`、模型/动态库随包、签名、公证、Gatekeeper 或发布机
Python-free 验收。这些工作由用户决定整体后置到未来发布阶段，不在开发分支保留半成品
脚手架。

## 2. 开发范围

| Feature | 内容 | 状态 |
|---|---|---|
| 06901 | `sublift_protocol` 与协议纯度 | done |
| 06902 | `sublift_application` 与 Worker Composition Root | done |
| 06903 | `sublift_pipeline` 从 core 拆分 | done |
| 06904 | 薄 Native CLI | done |
| 06905 | 目录与 public include 分层 | done |
| 06906 | Paddle 产品公共面 / diagnostics 分离 | done |
| 06907 | 开发期 `ModelBundle` Target 与路径模型 | done |
| 06908 | `ResourceLocator` 与 capability 单一来源 | done |
| 06909 | Native CLI 全引擎 parity | done |
| 06913 | fail-closed 默认、显式开发回滚与终态文档 | 验证后 done |

### 06907 边界

06907 只交付开发期模型路径对象、资源查找和 `probe == construct`。产品 manifest、
模型/ORT SHA、下载器、原子安装与许可证属于发布交付，不以开发缓存路径冒充分发实现。

## 3. 后置发布范围

| Feature | 后置内容 | 当前状态 |
|---|---|---|
| 06910 | macOS `.app`、模型/ORT/OpenCV/ffmpeg 随包、rpath、notices | blocked（用户决定后置） |
| 06911 | Developer ID、notarization、staple、Gatekeeper | blocked（用户决定后置） |
| 06912 | 发布 artifact 的无 Python 干净机产品门 | blocked（用户决定后置） |

后置意味着：

- 当前 CMake 不提供 `bundle` target；
- 当前仓库不保留伪完整的 bundle/signing/Python-free 发布脚本；
- SwiftPM 开发构建继续从开发 build 目录或显式 `SUBLIFT_WORKER_PATH` 查找 Worker；
- `docs/cpp/phase6.9-native-product-architecture.md` §13/§19 仅保留为未来发布验收定义；
- 后续恢复发布工作时重新立项，不复用未经严格验证的旧脚手架。

## 4. 架构硬约束

1. `core` 不依赖 JSON、UDS、ORT、OpenCV、Vision 或 ffmpeg 进程。
2. `pipeline` 只依赖 core 与 Ports。
3. `protocol` 的 public header 不暴露 nlohmann/json。
4. `application` 不构造具体 Adapter。
5. `worker` 装配 OCR、Extractor、Detector 与资源定位。
6. `ports/` 不 re-export 具体 Adapter。
7. CLI 不链接 ORT/OpenCV/Vision。
8. Python 默认不作为 C++ capability 缺失时的静默 fallback；显式
   `--runtime python` / `SUBLIFT_RUNTIME=python` 仍用于 Oracle 和开发回滚。

## 5. 验收

开发架构可合入当且仅当：

- [x] 06901–06909、06913 为 `done`；
- [x] 06910–06912 明确记录为用户后置且无实现脚手架混入；
- [x] CMake/public include/Ports/Adapters 符合第 4 节；
- [x] `git diff --check` 通过，golden/fixtures 无变化；
- [x] `./init.sh` 10/10；
- [x] runtime + live GT 扩展回归门通过；
- [x] Swift 测试通过；
- [x] `feature-list.json` 的开发架构块为 `done`，发布块保持后置。

完整 `.app` 分发是否完成不属于本验收结论。
