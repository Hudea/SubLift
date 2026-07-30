# feat-06910 — macOS Product Bundle（后置）

> 状态：**blocked / deferred**
> 决策：ADR-0030；用户于 2026-07-30 明确要求发布工作整体后置。

本 feature 不在 Phase 6.9 开发分支实现。未来发布阶段需重新设计并验收：

- `.app` 内 Worker、ORT、OpenCV、模型、ffmpeg/ffprobe 与 notices；
- model manifest/SHA、相对 rpath、断网 smoke；
- `otool -L` 无 Homebrew、venv 或开发机绝对路径；
- 可重复的 Release artifact 构建。

当前仓库不保留未满足上述门槛的 bundle 脚本或 CMake target。
