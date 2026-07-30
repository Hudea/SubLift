# Phase 6.7 C++ Paddle 路径检测后处理质量审计

- **日期：** 2026-07-29
- **审计范围：** Phase 6.7 `engine=paddle` + C++ runtime（`sublift_paddle`）
- **审计时状态：** 功能可运行，但检测后处理是 Native MVP，未达到 Python
  RapidOCR 全流程的实现完整度
- **后续处置：** 问题已由 Phase 6.8 的 `feat-06801`–`feat-06807` 修复并通过最终
  cutover；本文保留为问题发现时的历史基线，不代表当前实现状态

## 1. 问题结论

C++ Paddle 没有调用 RapidOCR Python 库，而是使用同一系列 PP-OCRv6 ONNX 模型和
自研 ONNX Runtime 流水线。Phase 6.7 的 Det 后处理只有阈值与连通域 AABB，不是
RapidOCR 的完整 DB/dilation/contour/score/unclip 实现。

因此，当时的 C++ Paddle 字幕框、识别文字和最终分段会系统性弱于或不同于 Python
RapidOCR。这首先是实现质量债，不只是门禁口径问题。

该问题不波及：

- Apple Vision 与 Mock 路径；
- Python `engine=paddle`，它仍使用 RapidOCR；
- 与 OCR adapter 无关的 signature、changepoint、timeline、dedupe 算法。

## 2. 两条 Paddle 路径

| 路径 | 实现 | 模型 | 审计时后处理 |
|---|---|---|---|
| Python | `src/sublift/ocr/paddle.py` → RapidOCR | PP-OCRv6，用户模型缓存 | RapidOCR 完整 Det + Cls + Rec |
| C++ | `cpp/src/paddle/paddle_ocr.cpp` → ONNX Runtime | 同系列 PP-OCRv6 ONNX | 自研阈值 + 连通域 AABB；无完整 DB unclip |

产品接口层两端都实现 SubLift 的 OCR engine 边界。RapidOCR 没有可直接链接的官方
C++ API，因此 Phase 6.7 选择原生实现，而不是嵌入 CPython 或通过 subprocess 冒充
Native。

```text
Python:
image → RapidOCR(Det 完整后处理 → Cls → Rec) → OcrLine → Pipeline

Phase 6.7 C++:
image → ORT Det probability map → 简化连通域 AABB → ORT Rec → OcrLine → Pipeline
                                ↑
                         主要质量差异来源
```

## 3. 技术差距

| 环节 | Python RapidOCR | Phase 6.7 C++ | 可能后果 |
|---|---|---|---|
| Det 网络 | PP-OCR Det ONNX | 同系列 ONNX | 网络特征可接近 |
| Det 后处理 | threshold、dilation、contour、score、unclip、过滤 | threshold + connected components + AABB | 框过紧、过松、漏框或粘连 |
| 无框语义 | 空结果 | 整图 fallback Rec | 胡认或噪声 |
| crop | 四角透视裁剪 | AABB crop | 倾斜文字形变、背景混入 |
| Cls | 方向分类与 180° 旋转 | 不完整 | 旋转字幕错识 |
| Rec | 动态宽度、padding、batch、模型字典、CTC | 基础 ORT Rec + CTC | 受框质量和预处理差异放大 |
| 当时 golden | 真实 RapidOCR 流程 | 手工几何/AABB 排序 | 可以“门绿但产品差” |

实现质量债的传播路径是：

```text
Det 算法不完整 → quad/AABB 不准 → crop 与方向判断改变 → Rec 错字/漏字
               → Pipeline 收到不同文本 → 条数、切段和最终字幕变化
```

## 4. 产品影响

| 现象 | 用户侧表现 |
|---|---|
| 框不准 | 错字、半截字、多余背景文字 |
| 漏框 | 缺字幕、usable recall 降低 |
| 粘连或过切 | 一条拆多条、错误合并、时间轴抖动 |
| 整图 fallback | 偶发胡认与假字幕 |

正向能力证据是：Release + ORT + PP-OCRv6 下，C++ Paddle 能对约 2 分钟样片导出
非空可读 SRT。这证明它不是空壳，但不能证明达到 RapidOCR 的质量上限和稳健性。

## 5. 验证缺口

当时以下门不能证明 C++ Paddle 已达产品质量：

| 门 | 为什么不足 |
|---|---|
| Vision GT L3 | 不经过 C++ Paddle Det |
| Mock / signature 等 golden | 不依赖 Paddle OCR |
| Paddle 纯几何 L0 | 只测 AABB/clamp/sort，不执行 Det/Cls/Rec |
| 默认 init | 没有强制真实 Paddle E2E 多源门 |
| 只写 `engine=paddle` 的报告 | 未区分 `runtime=python|cpp`，可能混报 |

用 RapidOCR 文本或 GT 对 C++ 做真实 E2E 硬门时，门红通常反映真实框/字差异；只跑
浅层门而显示绿色，则会形成假安全感。门禁和实现必须一起补齐。

## 6. 性能问题

审计时同一约 2 分钟样片：

- Python Paddle：约 `54.0s`，44 条；
- C++ Paddle：约 `147.8s`，43 条；
- C++/Python wall：约 `2.74x`。

30 秒 Mock 对照约为 Python `1.7s`、C++ `1.5s`，因此主要性能差距位于 Paddle
adapter，而不是 IPC 或通用 Pipeline。后续归因还发现当时 C++ ORT 被限制为单线程，
且不同来源的 ONNX Runtime 二进制即使版本相同，性能与浮点尾数也可能不同。

## 7. 当时的处置选择

### A. 补齐实现

- 完整实现 DB/unclip、quad crop、Cls、Rec 与 CTC；
- 用同一输入分阶段对比 Python RapidOCR 与 C++；
- 建立 runtime 明确的多源 Paddle E2E 门；
- 质量冻结后再做线程、batch、热点和分配优化；
- 全门通过后才允许默认切换 C++。

### B. 冻结 MVP

- Paddle 默认保持 Python，C++ 标记 experimental；
- 文档明确简化 Det 与边界；
- 禁止用 Vision 水位或浅层 golden 代表 C++ Paddle 产品质量。

项目选择了先执行 B 作为安全路由，再完整执行 A；对应 Phase 6.8
`feat-06801`–`feat-06807`。

## 8. 审计结论与后续闭环

| 问题 | 审计时答案 | Phase 6.8 闭环 |
|---|---|---|
| C++ 是否直接使用 RapidOCR？ | 否；同模型族的原生 ORT 流水线 | 保持 Native，但对齐冻结 RapidOCR 语义 |
| 是否影响实现质量？ | 是，仅 C++ Paddle | 完整 DB/Quad/Cls/Rec/CTC，stage fixtures 全过 |
| 是否影响质量门？ | 是；浅门可能假绿，真实门会暴露差距 | 3 来源/614.272s 真实双 runtime 门，输出 SHA exact |
| 是否达到性能要求？ | 否，约为 Python `2.74x` | 最终 wall `0.8956x`、RSS `0.9152x` |
| 能否作为产品默认？ | 当时不能 | 质量、性能、长流、取消、重启、回滚全过后已切换 |

最终状态与数值以
[Phase 6.8 最终验收报告](phase6.8-paddle-cutover.md)为准。
