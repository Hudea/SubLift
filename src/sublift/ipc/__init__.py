"""SubLift IPC 模块。

提供 Swift GUI 壳与 Python 核心之间的通信通道：
- `server.py`：Unix Domain Socket server，handle_connection 长连接多消息
- `protocol.py`：JSON 消息 schema（feat-015），8 类业务消息 + 2 类控制消息
- `bridge.py`：把 Phase 1 Pipeline 包成 IPC handler（feat-016）

feat-014：hello/bye 骨架握手验证通道可用。
feat-015：协议 schema + handler stub 响应分发。
feat-016：handler 接入真实 Pipeline（bridge.BridgeHandler）。
"""
