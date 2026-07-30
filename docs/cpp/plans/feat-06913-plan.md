# feat-06913 — 开发架构终态收口

> 状态：done
> 决策：ADR-0030

## 范围

- C++ Paddle unavailable 时默认 fail-closed，不静默启动 Python；
- 显式 `--runtime python` / `SUBLIFT_RUNTIME=python` 继续作为 Oracle 与开发回滚；
- protocol public header 不暴露 nlohmann/json；
- 删除 `sublift_ipc` 兼容 Target alias；
- `ports/` 只保留抽象接口；
- Apple Vision synthetic live OCR 从默认确定性 CTest 分离，固定视频 GT 继续作为扩展门；
- 删除当前分支的 bundle/signing/Python-free 发布脚手架；
- 同步 phase、feature-list、ADR、架构文档与 progress。

## Done

- [x] C++ 全量 CTest、Python pytest、ruff、mypy 与 parity 通过；
- [x] Swift 测试通过；
- [x] runtime + live GT 扩展回归通过；
- [x] `git diff --check` 通过，golden/fixtures 无变化；
- [x] 06913 与开发架构功能块标记 `done`；
- [x] 06910–06912 保持明确后置，不声明发布完成。
