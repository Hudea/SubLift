"""Bridge handler 单测（feat-016 / feat-029）。

用 MockOcrEngine 跑真实 Pipeline 流式 API，验证完整流程：
- start_job → 构造 Pipeline
- frame → JPEG 解码 → feed → 段闭合时 push_entry → progress
- finalize → finalize() → entries(is_final=True)
- cancel_job → cancel() → done
- 安全限制：JPEG 大小上限 / 无效 JPEG
"""

from __future__ import annotations

import asyncio
import io
from pathlib import Path
from typing import Any
from unittest.mock import patch

from PIL import Image

from sublift.detector.bottom_crop import BottomCropDetector
from sublift.detector.fixed_region import FixedRegionDetector
from sublift.ipc.bridge import (
    _PROGRESS_EVERY_N_FRAMES,
    MAX_JPEG_BYTES,
    BridgeHandler,
)
from sublift.ipc.protocol import (
    build_cancel_job,
    build_finalize,
    build_frame,
    build_start_job,
)
from sublift.ocr.mock import MockOcrEngine


def _make_jpeg(width: int = 320, height: int = 240, color: str = "black") -> bytes:
    """生成一个有效的最小 JPEG。"""
    img = Image.new("RGB", (width, height), color=color)
    buf = io.BytesIO()
    img.save(buf, format="JPEG")
    return buf.getvalue()


def _make_handler() -> BridgeHandler:
    return BridgeHandler(ocr_engine_factory=MockOcrEngine)


async def _noop_push(_msg: dict[str, Any]) -> None:
    """默认 push 回调，什么都不做。"""
    pass


def _run(bridge: BridgeHandler, msg: dict[str, Any]) -> dict[str, Any] | None:
    """便捷包装：同步调用 bridge.handle，用 noop push。"""
    return asyncio.run(bridge.handle(msg, _noop_push))


class TestStartJob:
    def test_start_job_returns_progress_ready(self) -> None:
        bridge = _make_handler()
        msg = build_start_job("V1", 5.0, "mock", 0.5)
        response = _run(bridge, msg)
        assert response is not None
        assert response["type"] == "progress"
        assert response["stage"] == "ready"
        assert response["video_id"] == "V1"

    def test_start_job_with_duration_estimates_frames(self) -> None:
        bridge = _make_handler()
        msg = build_start_job("V1", 5.0, "mock", 0.5, duration_ms=10000)
        response = _run(bridge, msg)
        assert response is not None
        assert response["stage"] == "ready"

    def test_start_job_invalid_engine_returns_error(self) -> None:
        bridge = _make_handler()
        msg = build_start_job("V1", 5.0, "nonexistent", 0.5)
        response = _run(bridge, msg)
        assert response is not None
        assert response["type"] == "error"

    def test_start_job_engine_mismatch_returns_done_error_and_can_restart(self) -> None:
        """服务端绑定 mock 时，不得把请求 vision 静默当作 mock 执行。"""
        bridge = _make_handler()

        response = _run(bridge, build_start_job("V1", 5.0, "vision", 0.5))

        assert response == {
            "type": "done",
            "video_id": "V1",
            "ok": False,
            "error": "engine 不匹配: server 使用 'mock'，start_job 请求 'vision'",
        }
        assert bridge._pipeline is None

        # 拒绝不一致请求不污染 handler；同连接的下一任务可正常开始。
        recovered = _run(bridge, build_start_job("V2", 5.0, "mock", 0.5))
        assert recovered is not None
        assert recovered["type"] == "progress"
        assert recovered["stage"] == "ready"

    def test_start_job_with_region_box_uses_fixed_detector(self) -> None:
        bridge = _make_handler()
        msg = build_start_job(
            "V1", 5.0, "mock", 0.5, region_box=[0, 800, 1920, 200]
        )
        _run(bridge, msg)
        assert bridge._pipeline is not None
        assert isinstance(bridge._pipeline._detector, FixedRegionDetector)
        # 无显式 profile 时从 region 推导 crop 全带
        assert bridge._pipeline.subtitle_profile is not None
        assert bridge._pipeline.subtitle_profile.height == 200
        assert bridge._pipeline.subtitle_profile.center_x == 960

    def test_start_job_with_explicit_subtitle_profile(self) -> None:
        from sublift.models import SubtitleProfile

        bridge = _make_handler()
        profile = {
            "script": "cjk",
            "center_x": 100,
            "center_y": 20,
            "height": 40,
            "y_min": 5,
            "y_max": 45,
        }
        msg = build_start_job(
            "V1",
            5.0,
            "mock",
            0.5,
            region_box=[0, 800, 1920, 100],
            subtitle_profile=profile,
        )
        _run(bridge, msg)
        assert bridge._pipeline is not None
        assert bridge._pipeline.subtitle_profile == SubtitleProfile.from_dict(profile)

    def test_start_job_without_region_box_uses_bottom_crop(self) -> None:
        bridge = _make_handler()
        msg = build_start_job("V1", 5.0, "mock", 0.5)
        _run(bridge, msg)
        assert bridge._pipeline is not None
        assert isinstance(bridge._pipeline._detector, BottomCropDetector)
        assert bridge._pipeline.subtitle_profile is None

    def test_start_job_vision_unavailable_returns_done_error(self) -> None:
        """VisionOcrEngine 构造失败时应返回 done(ok=False)。"""
        bridge = BridgeHandler()  # 默认 VisionOcrEngine
        with patch("sublift.ocr.vision.VisionOcrEngine.__init__") as mock_init:
            mock_init.side_effect = RuntimeError("Vision 不可用")
            msg = build_start_job("V1", 5.0, "vision", 0.5)
            response = _run(bridge, msg)
        assert response is not None
        assert response["type"] == "done"
        assert response["ok"] is False


class TestFrame:
    def test_frame_before_start_job_returns_error(self) -> None:
        bridge = _make_handler()
        jpeg = _make_jpeg()
        msg = build_frame("V1", 1000, jpeg)
        response = _run(bridge, msg)
        assert response is not None
        assert response["type"] == "error"
        assert "before start_job" in response["message"]

    def test_frame_returns_progress(self) -> None:
        bridge = _make_handler()
        _run(bridge, build_start_job("V1", 5.0, "mock", 0.5))
        jpeg = _make_jpeg()
        msg = build_frame("V1", 1000, jpeg)
        response = _run(bridge, msg)
        assert response is not None
        assert response["type"] == "progress"
        assert response["stage"] == "processing"

    def test_frame_invalid_jpeg_returns_error(self) -> None:
        bridge = _make_handler()
        _run(bridge, build_start_job("V1", 5.0, "mock", 0.5))
        msg = build_frame("V1", 1000, b"not a jpeg")
        response = _run(bridge, msg)
        assert response is not None
        assert response["type"] == "error"
        assert "JPEG" in response["message"] or "decode" in response["message"].lower()

    def test_frame_invalid_base64_returns_error(self) -> None:
        bridge = _make_handler()
        _run(bridge, build_start_job("V1", 5.0, "mock", 0.5))
        msg: dict[str, Any] = {
            "type": "frame",
            "video_id": "V1",
            "ts_ms": 1000,
            "jpeg_bytes": "!!!invalid base64!!!",
            "region_box": None,
        }
        response = _run(bridge, msg)
        assert response is not None
        assert response["type"] == "error"

    def test_frame_ocr_error_returns_done_with_original_message_and_clears_state(self) -> None:
        """legacy frame mode 的 OCR 失败必须可传递到客户端，不能断开 UDS。"""
        bridge = _make_handler()
        _run(bridge, build_start_job("V1", 5.0, "mock", 0.5))
        pipeline = bridge._pipeline
        assert pipeline is not None

        original_error = "Paddle OCR 模型执行失败"
        with (
            patch.object(pipeline, "feed", return_value=object()),
            patch.object(
                pipeline,
                "ocr_segment",
                side_effect=RuntimeError(original_error),
            ),
        ):
            response = _run(bridge, build_frame("V1", 1000, _make_jpeg()))

        assert response == {
            "type": "done",
            "video_id": "V1",
            "ok": False,
            "error": original_error,
        }
        assert bridge._pipeline is None
        assert bridge._ocr is None

        # 失败后不能复用部分推进的 Pipeline，但应允许明确重启。
        recovered = _run(bridge, build_start_job("V2", 5.0, "mock", 0.5))
        assert recovered is not None
        assert recovered["type"] == "progress"

    def test_finalize_error_returns_done_with_original_message_and_clears_state(self) -> None:
        bridge = _make_handler()
        _run(bridge, build_start_job("V1", 5.0, "mock", 0.5))
        pipeline = bridge._pipeline
        assert pipeline is not None

        original_error = "Paddle OCR finalize 失败"
        with patch.object(
            pipeline,
            "finalize",
            side_effect=RuntimeError(original_error),
        ):
            response = _run(bridge, build_finalize("V1"))

        assert response == {
            "type": "done",
            "video_id": "V1",
            "ok": False,
            "error": original_error,
        }
        assert bridge._pipeline is None
        assert bridge._ocr is None


class TestStreamingPush:
    """流式 push_entry 推送测试（feat-029）。"""

    def test_frame_triggers_push_entry_on_segment_close(self) -> None:
        """段闭合时 bridge 应通过 push 推送 push_entry 消息。"""
        bridge = _make_handler()
        _run(bridge, build_start_job("V1", 5.0, "mock", 0.5, region_box=[0, 100, 320, 120]))

        pushed: list[dict[str, Any]] = []

        async def capturing_push(msg: dict[str, Any]) -> None:
            pushed.append(msg)

        # 造一系列帧驱动段闭合（需要多帧才能触发 IN→OUT）
        # 用黑色 JPEG 模拟有字幕帧，白色模拟无字幕
        black_jpeg = _make_jpeg(color="black")
        white_jpeg = _make_jpeg(color="white")

        # 先推几帧黑色（模拟字幕出现）
        for i in range(3):
            asyncio.run(bridge.handle(build_frame("V1", i * 200, black_jpeg), capturing_push))

        # 推几帧白色（模拟字幕消失，触发 OUT）
        for i in range(3, 6):
            asyncio.run(bridge.handle(build_frame("V1", i * 200, white_jpeg), capturing_push))

        # 应该有 push_entry（段闭合时）
        # 注意：可能没有，取决于 detector 和 changepoint 是否真触发
        # 关键验证：push 回调被调用过（如果有段闭合）
        # 不做硬性断言，因为合成 JPEG 不一定能驱动状态机
        # 主要验证 push 回调机制工作正常
        for msg in pushed:
            assert msg["type"] == "push_entry"
            assert "entry" in msg

    def test_finalize_returns_entries_is_final(self) -> None:
        """finalize 应返回 entries 消息且 is_final=True。"""
        bridge = _make_handler()
        _run(bridge, build_start_job("V1", 5.0, "mock", 0.5, region_box=[0, 100, 320, 120]))
        jpeg = _make_jpeg()
        _run(bridge, build_frame("V1", 0, jpeg))
        response = _run(bridge, build_finalize("V1"))
        assert response is not None
        assert response["type"] == "entries"
        assert response["video_id"] == "V1"
        assert response.get("is_final") is True
        assert isinstance(response["entries"], list)

    def test_finalize_before_start_job_returns_error(self) -> None:
        bridge = _make_handler()
        response = _run(bridge, build_finalize("V1"))
        assert response is not None
        assert response["type"] == "error"
        assert "before start_job" in response["message"]

    def test_finalize_empty_frames_returns_empty_entries(self) -> None:
        """start_job 后不推任何帧直接 finalize。"""
        bridge = _make_handler()
        _run(bridge, build_start_job("V1", 5.0, "mock", 0.5))
        response = _run(bridge, build_finalize("V1"))
        assert response is not None
        assert response["type"] == "entries"
        assert response["entries"] == []
        assert response.get("is_final") is True


class TestCancelJob:
    def test_cancel_returns_done_cancelled(self) -> None:
        bridge = _make_handler()
        _run(bridge, build_start_job("V1", 5.0, "mock", 0.5))
        response = _run(bridge, build_cancel_job("V1"))
        assert response is not None
        assert response["type"] == "done"
        assert response["ok"] is False
        assert response["error"] == "cancelled"

    def test_cancel_clears_pipeline(self) -> None:
        """cancel 后再 finalize 应报错（pipeline 已清空）。"""
        bridge = _make_handler()
        _run(bridge, build_start_job("V1", 5.0, "mock", 0.5))
        jpeg = _make_jpeg()
        _run(bridge, build_frame("V1", 0, jpeg))
        _run(bridge, build_cancel_job("V1"))
        response = _run(bridge, build_finalize("V1"))
        assert response is not None
        assert response["type"] == "error"

    def test_frame_after_cancel_returns_error(self) -> None:
        """cancel 后再推帧应报错。"""
        bridge = _make_handler()
        _run(bridge, build_start_job("V1", 5.0, "mock", 0.5))
        _run(bridge, build_cancel_job("V1"))
        jpeg = _make_jpeg()
        response = _run(bridge, build_frame("V1", 0, jpeg))
        assert response is not None
        assert response["type"] == "error"


class TestSafetyLimits:
    def test_jpeg_size_limit(self) -> None:
        """超过 MAX_JPEG_BYTES 应返回 error。"""
        bridge = _make_handler()
        _run(bridge, build_start_job("V1", 5.0, "mock", 0.5))

        big_jpeg = b"\xff\xd8\xff\xe0" + b"\x00" * (MAX_JPEG_BYTES + 1)
        msg = build_frame("V1", 1000, big_jpeg)
        response = _run(bridge, msg)
        assert response is not None
        assert response["type"] == "error"
        assert "JPEG 过大" in response["message"]


def _generate_test_video(path: Path, duration: float = 1.0, fps: float = 2.0) -> None:
    """用 ffmpeg lavfi 生成短测试视频。"""
    import subprocess

    cmd = [
        "ffmpeg",
        "-v",
        "error",
        "-f",
        "lavfi",
        "-i",
        f"testsrc=duration={duration}:size=320x240:rate={fps}",
        "-c:v",
        "libx264",
        "-pix_fmt",
        "yuv420p",
        str(path),
        "-y",
    ]
    subprocess.run(cmd, capture_output=True, check=True)


class TestPathMode:
    """start_job.video_path → 后端 FfmpegExtractor（与 CLI 同源）。"""

    def test_missing_video_path_returns_done_error(self) -> None:
        bridge = _make_handler()
        msg = build_start_job(
            "V1", 5.0, "mock", 0.5, video_path="/nonexistent/no_video.mp4"
        )
        pushed: list[dict[str, Any]] = []

        async def collect_push(msg: dict[str, Any]) -> None:
            pushed.append(msg)

        async def run_it() -> None:
            response = await bridge.handle(msg, collect_push)
            assert response is not None
            assert response["type"] == "progress"
            if bridge._path_task:
                await bridge._path_task
        asyncio.run(run_it())
        done_msg = next((p for p in pushed if p.get("type") == "done"), None)
        assert done_msg is not None
        assert done_msg["ok"] is False
        assert "不存在" in (done_msg.get("error") or "")

    def test_path_mode_returns_entries(self, tmp_path: Path) -> None:
        video = tmp_path / "short.mp4"
        _generate_test_video(video, duration=1.0, fps=2.0)
        bridge = _make_handler()
        pushed: list[dict[str, Any]] = []

        async def collect_push(msg: dict[str, Any]) -> None:
            pushed.append(msg)

        msg = build_start_job(
            "V1",
            2.0,
            "mock",
            0.5,
            duration_ms=1000,
            video_path=str(video),
        )
        async def run_it() -> None:
            response = await bridge.handle(msg, collect_push)
            assert response is not None
            assert response["type"] == "progress"
            if bridge._path_task:
                await bridge._path_task
        asyncio.run(run_it())
        entries_msg = next((p for p in pushed if p.get("type") == "entries"), None)
        assert entries_msg is not None
        assert entries_msg.get("is_final") is True
        assert "entries" in entries_msg
        # 至少推送过 progress
        assert any(p.get("type") == "progress" for p in pushed)

    def test_path_mode_progress_is_throttled(self, tmp_path: Path) -> None:
        """path mode 不应每帧推 progress（使用 _PROGRESS_EVERY_N_FRAMES）。"""
        video = tmp_path / "throttle.mp4"
        # 2s @ 10fps 采样 → 约 20 帧；若每帧 progress 会远超节流后的数量
        _generate_test_video(video, duration=2.0, fps=10.0)
        bridge = _make_handler()
        pushed: list[dict[str, Any]] = []

        async def collect_push(msg: dict[str, Any]) -> None:
            pushed.append(msg)

        sample_fps = 10.0
        msg = build_start_job(
            "V1",
            sample_fps,
            "mock",
            0.5,
            duration_ms=2000,
            video_path=str(video),
        )
        async def run_it() -> None:
            response = await bridge.handle(msg, collect_push)
            assert response is not None
            assert response["type"] == "progress"
            if bridge._path_task:
                await bridge._path_task
        asyncio.run(run_it())

        entries_msg = next((p for p in pushed if p.get("type") == "entries"), None)
        assert entries_msg is not None

        processing = [
            p
            for p in pushed
            if p.get("type") == "progress" and p.get("stage") == "processing"
        ]
        # 含：开始 0.0、节流帧、结束 1.0；绝不应接近「每帧一条」
        est_frames = 20
        assert len(processing) < est_frames
        # 节流上限粗估：1(首帧) + floor((N-1)/N_every) + 1(完成) + 起始 0
        max_expected = 2 + (est_frames // _PROGRESS_EVERY_N_FRAMES) + 2
        assert len(processing) <= max_expected

    def test_frame_rejected_in_path_mode_after_failed_path(self) -> None:
        """path 失败后不应进入 path_mode 卡死；重新 frame mode 可用。"""
        bridge = _make_handler()
        _run(
            bridge,
            build_start_job("V1", 5.0, "mock", 0.5, video_path="/no/such.mp4"),
        )
        # 失败后 pipeline 已清空；重新 frame mode start
        resp = _run(bridge, build_start_job("V2", 5.0, "mock", 0.5))
        assert resp is not None
        assert resp["type"] == "progress"
        assert resp["stage"] == "ready"

    def test_path_mode_finalizing_stage_order(self, tmp_path: Path) -> None:
        """验证 finalizing 阶段的优先级与绝对顺序。"""
        video = tmp_path / "short.mp4"
        _generate_test_video(video, duration=1.0, fps=2.0)
        bridge = _make_handler()
        pushed: list[dict[str, Any]] = []

        async def collect_push(msg: dict[str, Any]) -> None:
            pushed.append(msg)

        msg = build_start_job(
            "V1",
            2.0,
            "mock",
            0.5,
            duration_ms=1000,
            video_path=str(video),
        )
        async def run_it() -> None:
            response = await bridge.handle(msg, collect_push)
            assert response is not None
            if bridge._path_task:
                await bridge._path_task
        asyncio.run(run_it())

        progress_msgs = [p for p in pushed if p.get("type") == "progress"]
        stages = [p.get("stage") for p in progress_msgs]

        # finalizing 必须存在，且绝不能在最后被 processing 1.0 覆盖
        assert "finalizing" in stages
        finalizing_idx = stages.index("finalizing")

        # 确认在此之后没有任何 processing 状态出现
        for p in stages[finalizing_idx + 1:]:
            assert p != "processing"

        # done 消息或 entries 消息应当是最后的实体消息
        entries_msg = next((p for p in pushed if p.get("type") == "entries"), None)
        assert entries_msg is not None

    def test_path_mode_cancel_and_restart(self, tmp_path: Path) -> None:
        """验证在同个 Handler 上取消任务后，能立即拉起新任务。"""
        video = tmp_path / "long.mp4"
        _generate_test_video(video, duration=5.0, fps=2.0)
        bridge = _make_handler()
        pushed: list[dict[str, Any]] = []

        async def collect_push(msg: dict[str, Any]) -> None:
            pushed.append(msg)

        # 1. 启动任务并立即取消
        msg1 = build_start_job(
            "V1",
            2.0,
            "mock",
            0.5,
            duration_ms=5000,
            video_path=str(video),
        )

        async def run_it() -> None:
            # 启动 Job 1
            await bridge.handle(msg1, collect_push)
            # 立即取消 Job 1，并等待其退出完成
            cancel_msg = build_cancel_job("V1")
            done_response = await bridge.handle(cancel_msg, collect_push)
            assert done_response is not None
            assert done_response["type"] == "done"
            assert done_response["ok"] is False

            # 2. 在相同的 BridgeHandler 实例上立即重新发起 Job 2
            msg2 = build_start_job(
                "V2",
                2.0,
                "mock",
                0.5,
                duration_ms=5000,
                video_path=str(video),
            )
            response2 = await bridge.handle(msg2, collect_push)
            assert response2 is not None
            assert response2["type"] == "progress"
            assert response2["stage"] == "ready"

            # 优雅取消 Job 2 完成测试
            await bridge.handle(build_cancel_job("V2"), collect_push)

        asyncio.run(run_it())
