# SubLift Contracts

> 状态：Phase 17 D01 done / D02 in-progress。本目录是 C++ / Swift / TypeScript 之间**唯一**的
> 跨语言语义定义处；产品代码可以有自己的 DTO，但字段语义、默认值、状态映射和 golden
> 字节必须由这里的 schema 与 fixtures 约束。D01 只建立合同；D02 开始逐项对齐产品行为。

## 1. 边界

- **属于合同**：ExtractionSpec、Job/Event、Task 状态映射、ROI 坐标转换、SRT 格式、
  输出冲突与重名、SaveResult、capability 的共享字段。
- **不属于合同**：SwiftUI/Vue 组件、窗口布局、平台文件面板、服务端路由实现、数据库/JSON
  存储实现、UI 文案。平台差异在 `docs/design_ui/shared/parity.md` 中显式标注。
- **单一方向**：合同不 import 任何产品代码；C++/Swift/TS 的实现与测试向合同靠拢。

## 2. 版本、所有权与变更

- 当前版本：`contracts/VERSION` = `1`。
- Owner：维护 Phase 的 D01/D02。任何跨端语义改动必须同时更新 schema/fixture 与至少一套
  消费测试；只改一端实现视为未完成。
- 向后兼容：新增可选字段不升 major；删除字段、改变默认值、改变状态含义、改变 SRT 字节
  或输出重名规则必须显式升级 `contracts/VERSION` 并更新 parity matrix。
- 禁止静默回退：engine 不可用、ROI 越界、损坏的队列状态、未知事件类型必须 fail-closed。

## 3. 文件

| 路径 | 内容 |
|---|---|
| `schemas/extraction.schema.json` | ExtractionSpec（engine/sampling/confidence/script/roi） |
| `schemas/job-event.schema.json` | JobEvent（job_id/seq/type/payload） |
| `schemas/job-snapshot.schema.json` | JobSnapshot（status/config/entries） |
| `VERSION` | 合同 major 版本（当前 1） |
| `schemas/output.schema.json` | OutputPlan（target/policy/conflict） |
| `schemas/save-result.schema.json` | SaveResult（saved/skipped/empty_result/failed） |
| `fixtures/defaults.json` | 规范默认值与 engine 偏好 |
| `fixtures/extraction-cases.json` | 合法 ExtractionSpec 样例 |
| `fixtures/roi-cases.json` | 归一化 box → 像素 box 的期望值 |
| `fixtures/srt-cases.json` | 规范 SRT 字节 |
| `fixtures/output-cases.json` | 冲突策略 → 输出名/状态 |
| `fixtures/job-events.json` / `job-snapshots.json` | Job/Event 样例 |
| `fixtures/save-results.json` | SaveResult 样例 |
| `fixtures/statuses.json` | Job/Task 状态与映射 |

## 4. 已冻结的规范语义（D01 基线）

- **采样**：`fast=5`、`balanced=8`、`fine=12` fps。
- **confidence**：规范默认 `0.0`，表示不做高置信过滤；不是缺省/未知。
- **engine**：偏好顺序 `vision > paddle`；`mock` 只能显式选择，不参与自动偏好。
- **ROI**：HTTP/UI 使用归一化 `[0,1]` box；Worker 使用像素 box。转换 = 四舍五入后
  clamp origin 到 `[0,size-1]`，再 clamp 宽高到剩余区域，最小 1。
- **SRT**：按 `start_ms` 升序；跳过纯空白文本；`start=max(0,start)`，
  `end=max(start,end)`；序号从 1 连续；块间空行；末尾保留一个空行（两个换行），
  与容器 golden 的精确比对保持一致。
- **输出冲突**：`skip` 不写；`deterministic_rename` 使用 `stem_1.srt`、`stem_2.srt`；
  `replace` 原子覆盖；空结果默认 `empty_result`，不落 0 字节文件。
- **状态**：Job 为 `queued/preparing/extracting/finalizing/completed/failed/cancelled/interrupted`；
  Task 为 `waiting/preparing/extracting/exporting/completed/failed/cancelled/interrupted/skipped`；
  `exporting` 映射 `finalizing`，`skipped` 没有 Job 对应态。

## 5. 校验

```bash
./scripts/verify-contracts.sh
```

脚本使用 stdlib Python 做 fixture 结构与参考算法校验；安装 `jsonschema` 时额外执行
Draft 2020-12 schema 校验。它是 D01/D02 的开发门，不是产品门；产品行为仍由
`./scripts/verify-product.sh` 与各语言测试锁定。
