"""SubLift IPC 模块。

提供 Swift GUI 壳与 Python 核心之间的通信通道：
- `server.py`：Unix Domain Socket server，接收 Swift 端连接与消息
- `bridge.py`（feat-016）：把 Phase 1 Pipeline 包成 IPC handler
- `protocol.py`（feat-015）：MsgPack 消息 schema

feat-014 阶段仅实现 hello/bye 骨架，验证 UDS 通道可用。
"""
