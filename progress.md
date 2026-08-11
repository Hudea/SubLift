# SubLift 进度导航

## 当前状态

- **最后更新：** 2026-08-10
- **当前 Phase：** Phase 9 Repository Stabilization and Cleanup 已完成。
- **进度真源：** `phases.json → detail_file`；本文件仅作会话导航。
- **下一优先：** 当前 main 无已登记的后续 Feature；如启动新范围，先登记新的 Phase / Feature，再实施。
- **当前计划：** `docs/plans/architecture/phase9-repository-cleanup.md`（Phase 9 已完成；证据见 `docs/phases/phase9.json`）。

## 进行中

- 无正在实施的 Feature；Phase 9 已收口。

## 近期完成

- [x] 09006：前序 evidence、入口与路径边界审计完成；标准产品门、96 个 Markdown 本地链接和 Phase 导航已收口。
- [x] 09005：安全本地产物入口已支持默认 dry-run、分类选择和路径保护；未执行真实本机清理。
- [x] 09004：benchmark canonical CLI、历史 shim、root fallback 与版本化资产边界已收口。
- [x] 09003：根目录手工脚本已审计，独有诊断已迁入专用边界，冗余入口已删除。
- [x] 09002：标准验证已切换到 `SUBLIFT_VERIFY_*`，与轻量 Harness init 命名解耦。

## 阻塞项 / 风险

- 无当前阻塞。独立 `.app`、签名/公证、多源 GT 与发布 artifact 属于未来范围；本轮标准门按默认策略跳过 runtime 性能与 GT L3，发布前仍须显式运行完整发布门。09005 未对当前电脑执行 `--apply`，未来真实清理仍须以 dry-run 清单取得用户再次明确授权。

## 近期决策

- ADR-0033：以 subtraction 模板精简活跃 Harness，校正运行时与历史 Phase 边界。
- ADR-0032：采用 Phase 索引与 legacy/canonical 兼容层；活跃 Harness 布局后由 ADR-0033 收缩。
- ADR-0031：Benchmark 代码进入正式包，版本化资产与本机产物分层。

> 本次完成证据见 `docs/phases/phase9.json`；历史功能证据仍在各 Phase detail 文件中。
