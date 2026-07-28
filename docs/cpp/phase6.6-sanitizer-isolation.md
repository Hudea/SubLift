# Phase 6.6 — Sanitizer 依赖隔离（feat-06607）

> 状态：**done（诊断隔离）**。任务事实源：`docs/phases/phase6.json`。

## 目标与范围

恢复 `SUBLIFT_ENABLE_OPENCV=OFF` 配置的 `sublift_worker` 可构建性，以获得不加载
OpenCV/TBB 的 ASan/UBSan 对照。该构建只用于诊断，**不是**产品 runtime，也不能处理
vision/mock 作业。

范围仅限 CMake target 选择、Worker capability/拒绝语义和验证证据；不改冻结的
signature、changepoint、Pipeline 或 OCR 算法，也不以“关闭 OpenCV”规避产品发布门。

## 设计

```text
SUBLIFT_ENABLE_OPENCV=ON   → bridge.cpp            → 完整 Pipeline Worker
SUBLIFT_ENABLE_OPENCV=OFF  → bridge_no_opencv.cpp  → 诊断 Worker
```

诊断 Worker 仍可接受 `hello`，但 `engines=[]`、`capabilities=[]`；任何 `start_job`
都以明确错误结束。这样客户端不会把“可启动”误判为“可处理视频”。`SUBLIFT_HAS_OPENCV`
由 `sublift_ipc` target 以公开编译定义传递，使 `EngineFactory` 与选中的 bridge 实现使用
同一能力事实。

## 验收

1. 正常 OpenCV 构建行为和 152 个 CTest 不回归。
2. `SUBLIFT_SANITIZE=ON` + `SUBLIFT_ENABLE_OPENCV=OFF` 的 Worker 可链接，`otool -L`
   不含 OpenCV/TBB，`--version` 退出码为 0。
3. 该诊断 Worker 的 `hello` 不宣告 engine/capability，`start_job` 明确拒绝。
4. 将相同的 `--version` 探针与 OpenCV ASan 构建复比，记录结论，不把依赖崩溃写成已修复。

## 结果（2026-07-29）

四项验收均已执行：正常 OpenCV CTest 152/152 通过；无 OpenCV 的 ASan Worker 不链接
OpenCV/TBB、`--version` 退出 0，且 `hello` 返回空 engine/capability、`start_job` 明确拒绝。
相同 ASan 配置重新链接 OpenCV 4.14（其依赖 TBB 2023.1.0）后，`--version` 仍退出 134，栈在
`tbb::detail::r1::__TBB_InitOnce` 退出析构。故本 feature 修复了 CMake 的“可选 OpenCV”假设，
并证明 sanitizer 故障依赖 OpenCV/TBB 链；它**没有**修复或豁免发布级 sanitizer 门。
