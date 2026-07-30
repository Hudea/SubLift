# feat-06911 — 签名、公证与 Gatekeeper（后置）

> 状态：**blocked / deferred**
> 决策：ADR-0030；用户于 2026-07-30 明确要求签名、公证工作整体后置。

未来发布阶段需在具备 Apple Developer 账号、Developer ID 证书和独立干净机时重新立项，
完成 hardened runtime、codesign、notarization、staple 与 Gatekeeper 真实视频验收。

开发阶段不使用 ad-hoc 签名或“缺证书降级通过”冒充发布完成。
