"""IPC 协议 schema 单测（feat-015）。

覆盖：
- 7 类业务消息 + 2 类控制消息的 build_*() 构造
- validate() 正常通过 + 边界非法（缺字段/类型错/枚举非法/region_box 长度≠4）
- 跨语言 fixture：用 hardcode 的 JSON 字节串（模拟 Swift 端产出）喂给 Python 解析 + validate
"""

import base64
import json

import pytest

from sublift.ipc.protocol import (
    MSG_CANCEL_JOB,
    MSG_DONE,
    MSG_ENTRIES,
    MSG_FRAME,
    MSG_HELLO,
    MSG_LOG,
    MSG_PROGRESS,
    MSG_START_JOB,
    ProtocolError,
    build_bye,
    build_cancel_job,
    build_done,
    build_entries,
    build_error,
    build_frame,
    build_hello,
    build_log,
    build_progress,
    build_start_job,
    validate,
)

VIDEO_ID = "ABC-1234"


class TestBuildAndValidate:
    """build_*() 构造的消息必须能通过 validate()。"""

    @pytest.mark.parametrize(
        "msg",
        [
            build_hello(),
            build_bye(),
            build_error("oops"),
            build_start_job(VIDEO_ID, 5.0, "vision", 0.5),
            build_start_job(VIDEO_ID, 5.0, "vision", 0.5, region_box=[10, 20, 100, 200]),
            build_frame(VIDEO_ID, 1000, b"\xff\xd8\xff\xe0fakejpeg"),
            build_frame(VIDEO_ID, 1000, b"\xff\xd8", region_box=[0, 0, 1920, 1080]),
            build_cancel_job(VIDEO_ID),
            build_progress(VIDEO_ID, "ready", 0.0, 0),
            build_entries(
                VIDEO_ID,
                [{"start_ms": 0, "end_ms": 1000, "text": "hello", "confidence": 0.9}],
            ),
            build_log(VIDEO_ID, "info", "started"),
            build_done(VIDEO_ID, True),
            build_done(VIDEO_ID, False, error="timeout"),
        ],
        ids=[
            "hello",
            "bye",
            "error",
            "start_job-no-region",
            "start_job-with-region",
            "frame-no-region",
            "frame-with-region",
            "cancel_job",
            "progress",
            "entries",
            "log",
            "done-ok",
            "done-error",
        ],
    )
    def test_build_then_validate(self, msg: dict[str, object]) -> None:
        validate(msg)


class TestStartJob:
    def test_start_job_fields(self) -> None:
        msg = build_start_job(VIDEO_ID, 5.0, "vision", 0.5, region_box=[10, 20, 100, 200])
        assert msg == {
            "type": MSG_START_JOB,
            "video_id": VIDEO_ID,
            "fps": 5.0,
            "engine": "vision",
            "confidence_threshold": 0.5,
            "region_box": [10, 20, 100, 200],
        }

    def test_start_job_region_box_none(self) -> None:
        msg = build_start_job(VIDEO_ID, 5.0, "vision", 0.5)
        assert msg["region_box"] is None

    def test_missing_video_id(self) -> None:
        msg = {"type": MSG_START_JOB, "fps": 5.0, "engine": "vision", "confidence_threshold": 0.5}
        with pytest.raises(ProtocolError, match="video_id"):
            validate(msg)

    def test_invalid_engine(self) -> None:
        msg = build_start_job(VIDEO_ID, 5.0, "invalid_engine", 0.5)
        with pytest.raises(ProtocolError, match="engine"):
            validate(msg)

    def test_missing_fps(self) -> None:
        msg = {
            "type": MSG_START_JOB,
            "video_id": VIDEO_ID,
            "engine": "vision",
            "confidence_threshold": 0.5,
        }
        with pytest.raises(ProtocolError, match="fps"):
            validate(msg)

    def test_region_box_wrong_length(self) -> None:
        msg = build_start_job(VIDEO_ID, 5.0, "vision", 0.5, region_box=[1, 2, 3])
        with pytest.raises(ProtocolError, match="region_box"):
            validate(msg)

    def test_region_box_non_int(self) -> None:
        msg = build_start_job(VIDEO_ID, 5.0, "vision", 0.5, region_box=[1, 2, "3", 4])  # type: ignore[list-item]
        with pytest.raises(ProtocolError, match="region_box"):
            validate(msg)


class TestFrame:
    def test_frame_fields(self) -> None:
        jpeg = b"\xff\xd8\xff\xe0\x00\x10JFIF"
        msg = build_frame(VIDEO_ID, 2000, jpeg)
        assert msg["type"] == MSG_FRAME
        assert msg["video_id"] == VIDEO_ID
        assert msg["ts_ms"] == 2000
        assert msg["jpeg_bytes"] == base64.b64encode(jpeg).decode("ascii")
        assert msg["region_box"] is None

    def test_missing_jpeg_bytes(self) -> None:
        msg = {"type": MSG_FRAME, "video_id": VIDEO_ID, "ts_ms": 1000}
        with pytest.raises(ProtocolError, match="jpeg_bytes"):
            validate(msg)

    def test_ts_ms_must_be_int_not_float(self) -> None:
        msg = {
            "type": MSG_FRAME,
            "video_id": VIDEO_ID,
            "ts_ms": 1000.0,
            "jpeg_bytes": "AAA",
        }
        with pytest.raises(ProtocolError, match="ts_ms"):
            validate(msg)

    def test_ts_ms_bool_rejected(self) -> None:
        """Python bool 是 int 子类，必须显式拒绝。"""
        msg = {
            "type": MSG_FRAME,
            "video_id": VIDEO_ID,
            "ts_ms": True,
            "jpeg_bytes": "AAA",
        }
        with pytest.raises(ProtocolError, match="ts_ms"):
            validate(msg)


class TestCancelJob:
    def test_cancel_job_fields(self) -> None:
        msg = build_cancel_job(VIDEO_ID)
        assert msg == {"type": MSG_CANCEL_JOB, "video_id": VIDEO_ID}


class TestProgress:
    def test_progress_fields(self) -> None:
        msg = build_progress(VIDEO_ID, "frame_received", 0.5, 3000)
        assert msg == {
            "type": MSG_PROGRESS,
            "video_id": VIDEO_ID,
            "stage": "frame_received",
            "pct": 0.5,
            "eta_ms": 3000,
        }

    def test_missing_stage(self) -> None:
        msg = {"type": MSG_PROGRESS, "video_id": VIDEO_ID, "pct": 0.0, "eta_ms": 0}
        with pytest.raises(ProtocolError, match="stage"):
            validate(msg)

    def test_eta_ms_must_be_int(self) -> None:
        msg = {
            "type": MSG_PROGRESS,
            "video_id": VIDEO_ID,
            "stage": "ready",
            "pct": 0.0,
            "eta_ms": 0.5,
        }
        with pytest.raises(ProtocolError, match="eta_ms"):
            validate(msg)


class TestEntries:
    def test_entries_with_multiple(self) -> None:
        entries = [
            {"start_ms": 0, "end_ms": 1000, "text": "hello", "confidence": 0.9},
            {"start_ms": 2000, "end_ms": 3000, "text": "world", "confidence": 0.8},
        ]
        msg = build_entries(VIDEO_ID, entries)
        validate(msg)
        assert msg["entries"] == entries

    def test_entries_missing_confidence(self) -> None:
        msg = {
            "type": MSG_ENTRIES,
            "video_id": VIDEO_ID,
            "entries": [{"start_ms": 0, "end_ms": 1000, "text": "hello"}],
        }
        with pytest.raises(ProtocolError, match="confidence"):
            validate(msg)

    def test_entries_text_not_str(self) -> None:
        msg = {
            "type": MSG_ENTRIES,
            "video_id": VIDEO_ID,
            "entries": [
                {"start_ms": 0, "end_ms": 1000, "text": 123, "confidence": 0.9}
            ],
        }
        with pytest.raises(ProtocolError, match="text"):
            validate(msg)

    def test_entries_not_list(self) -> None:
        msg = {
            "type": MSG_ENTRIES,
            "video_id": VIDEO_ID,
            "entries": "not a list",
        }
        with pytest.raises(ProtocolError, match="entries 非列表"):
            validate(msg)

    def test_entries_empty_list_ok(self) -> None:
        msg = build_entries(VIDEO_ID, [])
        validate(msg)


class TestLog:
    def test_log_fields(self) -> None:
        msg = build_log(VIDEO_ID, "warn", "low confidence")
        assert msg == {
            "type": MSG_LOG,
            "video_id": VIDEO_ID,
            "level": "warn",
            "msg": "low confidence",
        }

    def test_invalid_level(self) -> None:
        msg = build_log(VIDEO_ID, "trace", "msg")
        with pytest.raises(ProtocolError, match="level"):
            validate(msg)


class TestDone:
    def test_done_ok(self) -> None:
        msg = build_done(VIDEO_ID, True)
        validate(msg)
        assert msg["ok"] is True
        assert msg["error"] is None

    def test_done_with_error(self) -> None:
        msg = build_done(VIDEO_ID, False, error="ocr failed")
        validate(msg)
        assert msg["ok"] is False
        assert msg["error"] == "ocr failed"

    def test_ok_not_bool(self) -> None:
        msg = {"type": MSG_DONE, "video_id": VIDEO_ID, "ok": "yes"}
        with pytest.raises(ProtocolError, match="ok"):
            validate(msg)


class TestUnknownType:
    def test_unknown_type_raises(self) -> None:
        msg = {"type": "frobnicate", "video_id": VIDEO_ID}
        with pytest.raises(ProtocolError, match="未知消息类型"):
            validate(msg)

    def test_missing_type(self) -> None:
        msg = {"video_id": VIDEO_ID}
        with pytest.raises(ProtocolError, match="type"):
            validate(msg)


class TestHelloBye:
    def test_hello_validate(self) -> None:
        validate(build_hello("sublift-mac"))
        validate({"type": MSG_HELLO})

    def test_bye_validate(self) -> None:
        validate(build_bye())

    def test_error_validate(self) -> None:
        validate(build_error("something broke"))


class TestCrossLanguageFixtures:
    """跨语言 fixture：用 hardcode JSON 模拟 Swift 端产出的字节，喂给 Python 解析。

    这类测试的目的是验证：Swift Codable struct 的字段名与 Python protocol.py 完全对齐。
    字段名拼写/大小写错误是最常见的跨语言 bug，此处提前拦截。
    """

    def test_swift_start_job_fixture(self) -> None:
        """Swift JSONEncoder 产出的 start_job 字节（字段名对齐）。"""
        swift_json = json.dumps(
            {
                "type": "start_job",
                "video_id": "UUID-ABCD",
                "fps": 5.0,
                "engine": "vision",
                "confidence_threshold": 0.5,
                "region_box": None,
            },
            ensure_ascii=False,
        ).encode("utf-8")
        msg = json.loads(swift_json)
        validate(msg)

    def test_swift_frame_fixture(self) -> None:
        """Swift 端 JPEG base64 编码后送来的 frame 消息。"""
        swift_json = json.dumps(
            {
                "type": "frame",
                "video_id": "UUID-ABCD",
                "ts_ms": 5000,
                "jpeg_bytes": "/9j/4AAQSkZJRg==",
                "region_box": None,
            },
            ensure_ascii=False,
        ).encode("utf-8")
        msg = json.loads(swift_json)
        validate(msg)

    def test_python_progress_fixture_swift_consumes(self) -> None:
        """Python 构造的 progress 消息，Swift JSONDecoder 必须能消费。

        此处验证 Python 产出的 JSON 字段名与 Swift Codable struct 一致。
        """
        msg = build_progress("UUID-ABCD", "ready", 0.0, 0)
        body = json.dumps(msg, ensure_ascii=False)
        parsed = json.loads(body)
        assert parsed["type"] == "progress"
        assert parsed["video_id"] == "UUID-ABCD"
        assert parsed["stage"] == "ready"
        assert parsed["pct"] == 0.0
        assert parsed["eta_ms"] == 0

    def test_python_entries_fixture_swift_consumes(self) -> None:
        """Python 构造的 entries 消息（嵌套 array of dict），Swift 必须能解码。

        这正是 MsgPack 库翻车的场景：嵌套 array of struct。
        JSON 不会有这个 bug，但仍需验证字段名对齐。
        """
        msg = build_entries(
            "UUID-ABCD",
            [
                {"start_ms": 0, "end_ms": 1000, "text": "你好世界", "confidence": 0.95},
                {"start_ms": 2000, "end_ms": 3000, "text": "Hello World", "confidence": 0.85},
            ],
        )
        body = json.dumps(msg, ensure_ascii=False)
        parsed = json.loads(body)
        validate(parsed)
        assert len(parsed["entries"]) == 2
        assert parsed["entries"][0]["text"] == "你好世界"

    def test_python_done_fixture_swift_consumes(self) -> None:
        msg = build_done("UUID-ABCD", True)
        body = json.dumps(msg, ensure_ascii=False)
        parsed = json.loads(body)
        assert parsed["ok"] is True
        assert parsed["error"] is None
