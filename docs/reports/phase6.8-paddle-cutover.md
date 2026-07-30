# Phase 6.8 Paddle C++ 产品 Cutover 验收

- **回顾索引：** [设计、逐项修改、ADR、障碍与复跑入口](../cpp/phase6.8-review-index.md)
- **日期：** 2026-07-30
- **结论：** PASS
- **产品策略：** Paddle available → C++ stable；unavailable → Python Paddle
  `paddle_override`；显式 `runtime=python` 保留回滚
- **Worker SHA256：** `2446b1d74e91abd66e88a835d26294c09e6e899ebb5af9a385a6c87424d5c16a`
- **Bundled ORT SHA256：** `cadd9517e089d197e5d381bc31813875eb8addce8712bac501645fea8008800c`
- **模型：** PP-OCRv6 small

## 算子、质量与性能

| 门 | 结果 |
|---|---|
| Det | input/probability/quad exact；9/9 box P/R=1.0 |
| Crop / Cls / Rec | 固定 quad 后 tensor exact；CTC/text/order exact |
| 多源质量 | 3 来源、614.272s；逐源 SRT SHA exact，全部指标 delta=0 |
| 120s canonical wall | Python 50.700s / C++ 45.407s；`0.8956x` |
| 进程树 RSS | Python 1864.406MiB / C++ 1706.297MiB；`0.9152x` |

## 产品路由与回滚

| 顺序 | Runtime | Wall | Entries | SRT SHA256 |
|---|---:|---:|---:|---|
| 默认 C++（回滚前） | cpp | 4.615s | 10 | `44a8b52054d5020981592ff8bf2498df5897dfd7a0d02d8c1cac0b7add65237b` |
| 强制 Python 回滚 | python | 5.097s | 10 | `44a8b52054d5020981592ff8bf2498df5897dfd7a0d02d8c1cac0b7add65237b` |
| 默认 C++ restart | cpp | 4.491s | 10 | `44a8b52054d5020981592ff8bf2498df5897dfd7a0d02d8c1cac0b7add65237b` |

三次产品入口输出字节级一致，且日志分别公开 `runtime=cpp|python`、模型与
`stable` 状态。

## 长流与生命周期

- 连续 source duration：720.000s
- C++ 产品 wall：15.522s
- 输出：40 entries
- 真实 Paddle path job 在 progress>0 后 cancel：2.8ms（门 ≤1s）
- 同一 Worker restart readiness：0.1ms（门 ≤5s）

## 工程验证

- `./init.sh`：10/10 PASS
- Release C++ `[paddle]`：22 cases / 409 assertions PASS
- Swift：34 XCTest + 139 Swift Testing PASS；含 live 默认 C++ Paddle 握手与
  `SUBLIFT_CPP_PADDLE=0` fallback
- Python 路由/CLI：51 tests PASS
- ruff / strict mypy：PASS
- Release 全量 CTest：174/175；唯一失败为此前已知的 Apple Vision synthetic OCR
  环境用例。Paddle、core、worker 和标准 Debug 门均通过，该 Vision 项不计入 Paddle
  cutover 的通过范围。

## 构建可复现性与剩余边界

CMake 将已选择且已验收的 ORT 复制到 build `lib/`，Worker/CLI/trace/tests 通过
`@loader_path/../lib` 加载。删除虚拟环境的 `libonnxruntime.1.dylib` 软链后，Paddle
probe、测试和完整 cutover gate 仍通过。

当前完成的是开发产品路径，不代表 6.9+ 分发已完成：正式 `.app` 内置模型/ORT、
codesign/notarization、universal2 与 Windows/Linux 安装包仍需独立 feature。Python
Paddle 至少保留一个小版本周期。
