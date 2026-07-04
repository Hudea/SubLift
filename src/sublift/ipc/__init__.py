"""SubLift IPC 模块。

提供 Swift GUI 壳与 Python 核心之间的通信通道：
- `server.py`：Unix Domain Socket server，9 类消息分发（7 业务 + 2 控制）
- `protocol.py`：JSON 消息 schema（feat-015），7 类业务消息 + 2 类控制消息
- `bridge.py`（feat-016）：把 Phase 1 Pipeline 包成 IPC handler

feat-014：hello/bye 骨架握手验证通道可用。
feat-015：协议 schema + handler stub 响应分发。
feat-016：handler 接入真实 Pipeline。
"""
