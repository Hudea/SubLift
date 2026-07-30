# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-30
- **当前 Phase：** Phase 6.8 Paddle Native hardening 已完成；6.9+ 尚未开始
- **已完成：** feat-06801–feat-06807（安全路由、算子 parity、多源质量、性能、产品 cutover）
- **产品默认 Runtime：** `engine=paddle` available → C++ stable；unavailable → Python Paddle `paddle_override`
- **回滚：** `SUBLIFT_RUNTIME=python` / `SUBLIFT_CPP_PADDLE=0`
- **回顾入口：** `docs/cpp/phase6.8-review-index.md`

## 进行中

- 无；下一步需从 6.9+ 新建独立 feature，不能把打包范围混入已完成的 6.8。

## 近期完成

- [x] **feat-06807**：Paddle available 默认 C++ stable；720s 长流、2.8ms cancel、0.1ms restart 与 C++→Python→C++ exact 回滚通过；GUI path-mode 日志已按实际 runtime/extractor 显示
- [x] **feat-06806**：真实 120s canonical 产品门 C++/Python wall=`0.8956x`、RSS=`0.9152x`；性能后 3 来源质量仍逐源 SHA exact
- [x] **feat-06805**：真实 3 来源/614.272s Q2 产品门；pyclipper round-unclip 修复后逐源 SRT hash、全部质量指标与 Q0 boxes 均 Python/C++ exact
- [x] **feat-06804**：真实 Cls/180°、动态 Rec batch、metadata 字典、CTC rounding；固定 quad 后 crop/Cls/Rec tensor exact，9/9 最终文本与顺序 exact
- [x] **feat-06803**：Det tensor exact；同 ORT probability exact；跨构建误差有 SHA 指纹；box P/R=1.0、≤1px；完整 DB/empty 门通过

## 阻塞项 / 风险

- Q2 当前只有 1 个真实外置片源 + 2 个可确定生成源；足以冻结本轮多样式相对门，但不代表广泛真实片源覆盖。
- canonical Candidate 固定官方 ORT SHA `cadd9517…`；build tree 已相对 rpath 自带该 dylib，但正式 `.app` 模型/签名/公证仍属 6.9+。
- Release 全量 CTest 174/175；唯一失败为既有 Apple Vision synthetic OCR 用例，标准 Debug `./init.sh` 10/10，Paddle Release 22/22。

## 近期决策

- **ADR-0029**：Paddle 全门通过后默认 C++ stable；ORT 自带相对 rpath，Python 保留回滚
- **ADR-0028**：性能门固定 ORT 二进制；拒绝导致 SRT hash 漂移的 batch=1 微优化
- **ADR-0027**：Paddle E2E 门必须真实运行、多源指纹化；1px Det 几何不得以 Cls 阈值掩盖

> 完整完成证据见 `docs/phases/phase6.json`；长期决策见 `docs/DECISIONS.md`。
