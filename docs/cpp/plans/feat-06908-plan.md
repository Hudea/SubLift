# feat-06908 — ResourceLocator 与 Probe=Construct

> 状态：done
> 范围修订：ADR-0030

## 目标

建立开发期统一资源定位器，使 Paddle 可用性探查与实际构造共用同一条解析链，
并让 FFmpeg/ffprobe、ONNX Runtime 的显式路径处理具备一致、可诊断的结果类型。

## 开发阶段查找边界

- Paddle 模型：显式参数 → `SUBLIFT_PADDLE_MODEL_DIR` → 用户缓存。
- FFmpeg/ffprobe：显式参数 → 对应环境变量 → `PATH` → 常见系统安装路径。
- ONNX Runtime：显式参数 → `SUBLIFT_ORT_LIB_DIR`。
- 查找结果由 `ResourceResult<T>` 表达，包含 `found`、`value`、`source` 和
  `error_msg`。
- 显式指定不存在或不可执行的资源时 fail-closed，不静默回退。

`.app/Contents/Resources`、`.app/Contents/Frameworks`、随包模型/二进制和发布
manifest/SHA 均属于后置分发范围，不在开发态 `ResourceLocator` 中实现。

## 完成内容

- [x] `sublift_models` 提供 `ModelBundle`/`ModelPaths` 与 `ResourceLocator`。
- [x] `PaddleOcrEngine` 与 availability probe 共用模型解析和校验。
- [x] Paddle 缺模型时保留明确的 fail-closed 错误。
- [x] FFmpeg/ffprobe 统一处理 override、环境变量与系统路径。
- [x] Catch2 覆盖 override、缺失资源和 Probe=Construct。
- [x] 发布期资源布局按 ADR-0030 后置。

## 验证

```bash
ctest --test-dir build/cpp -R resource_locator
./init.sh
```

最终验证证据记录在 `docs/phases/phase6.json` 的 `feat-06908`。
