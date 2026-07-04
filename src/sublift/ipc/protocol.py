"""SubLift IPC 消息协议 schema（JSON 序列化）。

7 类业务消息 + 2 类握手控制消息，字段名 Python/Swift 双端对齐。
分帧由 server.py 的 read_message/write_message 负责（4 字节大端长度前缀 + UTF-8 JSON body），
本模块只管消息体 schema：构造、校验、字段常量。

消息总览（plan §4.1）：
    Swift → Python:
        start_job   启动提取任务
        frame       推送一帧 JPEG
        cancel_job  取消任务
    Python → Swift:
        progress    进度通知
        entries     批量字幕条目
        log         日志
        done        任务结束
    控制消息（feat-014 骨架，保留）:
        hello       握手发起
        bye         握手响应
"""

from __future__ import annotations

from typing import Any

type VideoId = str

# 消息类型常量（Swift 端 MessageType enum 对齐）
MSG_START_JOB = "start_job"
MSG_FRAME = "frame"
MSG_CANCEL_JOB = "cancel_job"
MSG_PROGRESS = "progress"
MSG_ENTRIES = "entries"
MSG_LOG = "log"
MSG_DONE = "done"
MSG_HELLO = "hello"
MSG_BYE = "bye"
MSG_ERROR = "error"

# engine 合法值
ENGINES = frozenset({"vision", "paddle"})

# log level 合法值
LOG_LEVELS = frozenset({"debug", "info", "warn", "error"})


class ProtocolError(ValueError):
    """消息 schema 校验失败。"""


def build_hello(client: str = "sublift-mac") -> dict[str, Any]:
    """构造 hello 握手消息。"""
    return {"type": MSG_HELLO, "client": client}


def build_bye() -> dict[str, Any]:
    """构造 bye 握手响应。"""
    return {"type": MSG_BYE}


def build_error(message: str) -> dict[str, Any]:
    """构造 error 响应。"""
    return {"type": MSG_ERROR, "message": message}


def build_start_job(
    video_id: VideoId,
    fps: float,
    engine: str,
    confidence_threshold: float,
    region_box: list[int] | None = None,
) -> dict[str, Any]:
    """构造 start_job 消息。

    Args:
        video_id: 任务标识（Swift 端 UUID.uuidString）。
        fps: 采样帧率。
        engine: OCR 引擎名（"vision" / "paddle"）。
        confidence_threshold: OCR 置信度阈值。
        region_box: 可选字幕区域 [x, y, width, height]，None 用默认检测。
    """
    return {
        "type": MSG_START_JOB,
        "video_id": video_id,
        "fps": fps,
        "engine": engine,
        "confidence_threshold": confidence_threshold,
        "region_box": region_box,
    }


def build_frame(
    video_id: VideoId,
    ts_ms: int,
    jpeg_bytes: bytes,
    region_box: list[int] | None = None,
) -> dict[str, Any]:
    """构造 frame 消息。

    Args:
        video_id: 任务标识。
        ts_ms: 帧时间戳（毫秒）。
        jpeg_bytes: JPEG 图像字节（base64 编码后放入 JSON）。
        region_box: 可选区域覆盖 [x, y, width, height]。
    """
    import base64

    return {
        "type": MSG_FRAME,
        "video_id": video_id,
        "ts_ms": ts_ms,
        "jpeg_bytes": base64.b64encode(jpeg_bytes).decode("ascii"),
        "region_box": region_box,
    }


def build_cancel_job(video_id: VideoId) -> dict[str, Any]:
    """构造 cancel_job 消息。"""
    return {"type": MSG_CANCEL_JOB, "video_id": video_id}


def build_progress(
    video_id: VideoId,
    stage: str,
    pct: float,
    eta_ms: int,
) -> dict[str, Any]:
    """构造 progress 消息。

    Args:
        video_id: 任务标识。
        stage: 阶段名（feat-016 接入 Pipeline 后收敛枚举）。
        pct: 进度百分比 0.0~1.0。
        eta_ms: 预计剩余时间（毫秒）。
    """
    return {
        "type": MSG_PROGRESS,
        "video_id": video_id,
        "stage": stage,
        "pct": pct,
        "eta_ms": eta_ms,
    }


def build_entries(
    video_id: VideoId,
    entries: list[dict[str, Any]],
) -> dict[str, Any]:
    """构造 entries 消息。

    Args:
        video_id: 任务标识。
        entries: 字幕条目列表，每项含 start_ms/end_ms/text/confidence。
    """
    return {
        "type": MSG_ENTRIES,
        "video_id": video_id,
        "entries": entries,
    }


def build_log(
    video_id: VideoId,
    level: str,
    msg: str,
) -> dict[str, Any]:
    """构造 log 消息。

    Args:
        video_id: 任务标识。
        level: 日志级别（debug/info/warn/error）。
        msg: 日志内容。
    """
    return {
        "type": MSG_LOG,
        "video_id": video_id,
        "level": level,
        "msg": msg,
    }


def build_done(
    video_id: VideoId,
    ok: bool,
    error: str | None = None,
) -> dict[str, Any]:
    """构造 done 消息。

    Args:
        video_id: 任务标识。
        ok: 是否成功完成。
        error: 失败时的错误描述。
    """
    return {
        "type": MSG_DONE,
        "video_id": video_id,
        "ok": ok,
        "error": error,
    }


def validate(message: dict[str, Any]) -> None:
    """校验消息 schema，不合法抛 ProtocolError。

    Args:
        message: 已解析的 JSON 字典。
    """
    msg_type = message.get("type")
    if not isinstance(msg_type, str):
        raise ProtocolError(f"type 缺失或非字符串: {msg_type!r}")

    if msg_type == MSG_HELLO:
        _require_str(message, "client", required=False, default="")
        return
    if msg_type == MSG_BYE:
        return
    if msg_type == MSG_ERROR:
        _require_str(message, "message")
        return

    _require_str(message, "video_id")

    if msg_type == MSG_START_JOB:
        _require_float(message, "fps")
        _require_str(message, "engine")
        if message["engine"] not in ENGINES:
            raise ProtocolError(f"engine 非法: {message['engine']!r}")
        _require_float(message, "confidence_threshold")
        _optional_region_box(message)
    elif msg_type == MSG_FRAME:
        _require_int(message, "ts_ms")
        _require_str(message, "jpeg_bytes")
        _optional_region_box(message)
    elif msg_type == MSG_CANCEL_JOB:
        pass
    elif msg_type == MSG_PROGRESS:
        _require_str(message, "stage")
        _require_float(message, "pct")
        _require_int(message, "eta_ms")
    elif msg_type == MSG_ENTRIES:
        _require_entries(message)
    elif msg_type == MSG_LOG:
        _require_str(message, "level")
        if message["level"] not in LOG_LEVELS:
            raise ProtocolError(f"level 非法: {message['level']!r}")
        _require_str(message, "msg")
    elif msg_type == MSG_DONE:
        _require_bool(message, "ok")
        if "error" in message and message["error"] is not None:
            _require_str(message, "error")
    else:
        raise ProtocolError(f"未知消息类型: {msg_type!r}")


def _require_str(
    message: dict[str, Any], key: str, *, required: bool = True, default: str = ""
) -> None:
    if key not in message:
        if required:
            raise ProtocolError(f"{key} 缺失")
        message.setdefault(key, default)
        return
    if not isinstance(message[key], str):
        raise ProtocolError(f"{key} 非字符串: {message[key]!r}")


def _require_int(message: dict[str, Any], key: str) -> None:
    if key not in message:
        raise ProtocolError(f"{key} 缺失")
    val = message[key]
    if not isinstance(val, int) or isinstance(val, bool):
        raise ProtocolError(f"{key} 非整数: {val!r}")


def _require_float(message: dict[str, Any], key: str) -> None:
    if key not in message:
        raise ProtocolError(f"{key} 缺失")
    val = message[key]
    if isinstance(val, bool) or not isinstance(val, (int, float)):
        raise ProtocolError(f"{key} 非数值: {val!r}")


def _require_bool(message: dict[str, Any], key: str) -> None:
    if key not in message:
        raise ProtocolError(f"{key} 缺失")
    if not isinstance(message[key], bool):
        raise ProtocolError(f"{key} 非布尔: {message[key]!r}")


def _optional_region_box(message: dict[str, Any]) -> None:
    if "region_box" not in message or message["region_box"] is None:
        return
    box = message["region_box"]
    if not isinstance(box, list) or len(box) != 4:
        raise ProtocolError(f"region_box 非四元数组: {box!r}")
    for v in box:
        if not isinstance(v, int) or isinstance(v, bool):
            raise ProtocolError(f"region_box 含非整数: {v!r}")


def _require_entries(message: dict[str, Any]) -> None:
    if "entries" not in message:
        raise ProtocolError("entries 缺失")
    entries = message["entries"]
    if not isinstance(entries, list):
        raise ProtocolError(f"entries 非列表: {entries!r}")
    for i, e in enumerate(entries):
        if not isinstance(e, dict):
            raise ProtocolError(f"entries[{i}] 非字典: {e!r}")
        for field in ("start_ms", "end_ms"):
            if field not in e or not isinstance(e[field], int) or isinstance(e[field], bool):
                raise ProtocolError(f"entries[{i}].{field} 缺失或非整数: {e.get(field)!r}")
        if "text" not in e or not isinstance(e["text"], str):
            raise ProtocolError(f"entries[{i}].text 缺失或非字符串: {e.get('text')!r}")
        if "confidence" not in e or not isinstance(e["confidence"], (int, float)) or isinstance(
            e["confidence"], bool
        ):
            raise ProtocolError(f"entries[{i}].confidence 缺失或非数值: {e.get('confidence')!r}")
