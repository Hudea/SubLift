# feat-06912 — Python-free 发布产品门（后置）

> 状态：**blocked / deferred**
> 决策：ADR-0030；用户于 2026-07-30 明确要求发布工作整体后置。

未来发布阶段必须针对最终 artifact 验证零 Python/uv/.venv、零外部开发路径、真实
视频到 SRT、cancel/restart/长流与 6.8 质量性能不退化。

开发期 Native CLI 走 C++ Worker、默认 fail-closed 的行为由 06909/06913 验证，但不能
替代最终分发 artifact 的 Python-free 声明。
