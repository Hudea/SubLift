"""feat-038：固定区域 ROI 输出通路回归。"""

from __future__ import annotations

import json
import subprocess
from pathlib import Path
from typing import Any
from unittest.mock import patch

import pytest
from PIL import Image

from sublift.config import Config
from sublift.detector import FixedRegionDetector, RoiPassthroughDetector
from sublift.detector.base import Detector
from sublift.detector.bottom_crop import BottomCropDetector
from sublift.diagnostics.performance import PerformanceMode, PerformanceRecorder
from sublift.extractor.ffmpeg_extractor import (
    FfmpegExtractor,
    SourceFrameInfo,
    _assess_display_transform,
    validate_output_crop,
)
from sublift.extractor.frame_io import build_output_vf, plan_frame_io
from sublift.models import BoundingBox, Frame, SubtitleProfile
from sublift.ocr import MockOcrEngine
from sublift.pipeline import Pipeline

# ---------------------------------------------------------------------------
# 坐标校验
# ---------------------------------------------------------------------------


class TestValidateOutputCrop:
    def test_validate_output_crop_accepts_edge_fit(self) -> None:
        validate_output_crop(BoundingBox(0, 848, 1920, 232), 1920, 1080)

    def test_validate_output_crop_rejects_negative_xy(self) -> None:
        with pytest.raises(ValueError, match="source=1920x1080") as ei:
            validate_output_crop(BoundingBox(-1, 0, 10, 10), 1920, 1080)
        assert "requested=" in str(ei.value)

    def test_validate_output_crop_rejects_zero_size(self) -> None:
        with pytest.raises(ValueError, match="必须为正"):
            validate_output_crop(BoundingBox(0, 0, 0, 10), 100, 100)

    def test_validate_output_crop_rejects_overflow(self) -> None:
        with pytest.raises(ValueError, match="越界") as ei:
            validate_output_crop(BoundingBox(0, 1000, 1920, 100), 1920, 1080)
        msg = str(ei.value)
        assert "source=1920x1080" in msg
        assert "[0, 1000, 1920, 100]" in msg

    def test_validate_output_crop_allows_odd_dimensions(self) -> None:
        validate_output_crop(BoundingBox(1, 1, 101, 51), 320, 240)


class TestDisplayTransform:
    def test_probe_display_transform_ok_without_rotate(self) -> None:
        ok, note = _assess_display_transform({"width": 320, "height": 240})
        assert ok is True
        assert note is None

    def test_probe_display_transform_not_ok_with_rotate(self) -> None:
        ok, note = _assess_display_transform(
            {"width": 320, "height": 240, "tags": {"rotate": "90"}}
        )
        assert ok is False
        assert note is not None
        assert "rotate" in note

    def test_identity_display_matrix_rotation_zero(self) -> None:
        from sublift.extractor.ffmpeg_extractor import _is_identity_display_matrix

        assert _is_identity_display_matrix({"side_data_type": "Display Matrix", "rotation": 0})
        assert _is_identity_display_matrix(
            {
                "side_data_type": "Display Matrix",
                "rotation": 0.0,
                # 标准 16.16 + a33=2^30 恒等矩阵
                "displaymatrix": ("65536 0 0 0 65536 0 0 0 1073741824"),
            }
        )

    def test_identity_display_matrix_fixed_point_without_rotation_field(
        self,
    ) -> None:
        from sublift.extractor.ffmpeg_extractor import _is_identity_display_matrix

        assert _is_identity_display_matrix(
            {
                "side_data_type": "Display Matrix",
                "displaymatrix": [65536, 0, 0, 0, 65536, 0, 0, 0, 1073741824],
            }
        )

    def test_non_identity_display_matrix_rejected(self) -> None:
        from sublift.extractor.ffmpeg_extractor import _is_identity_display_matrix

        assert not _is_identity_display_matrix({"side_data_type": "Display Matrix", "rotation": 90})
        # 误用 a33=65536 的伪恒等矩阵必须拒绝（轮1 Major 防护）
        assert not _is_identity_display_matrix(
            {
                "side_data_type": "Display Matrix",
                "displaymatrix": [65536, 0, 0, 0, 65536, 0, 0, 0, 65536],
            }
        )
        ok, _ = _assess_display_transform(
            {
                "width": 320,
                "height": 240,
                "side_data_list": [{"side_data_type": "Display Matrix", "rotation": 90}],
            }
        )
        assert ok is False


# ---------------------------------------------------------------------------
# Detector
# ---------------------------------------------------------------------------


class TestRoiPassthroughDetector:
    def test_roi_passthrough_is_detector(self) -> None:
        assert isinstance(RoiPassthroughDetector(1920, 87), Detector)

    def test_roi_passthrough_returns_full_local_box(self) -> None:
        det = RoiPassthroughDetector(1920, 87)
        frame = Frame(timestamp_ms=0, image=Image.new("RGB", (1920, 87)))
        region = det.detect(frame)
        assert region.box == BoundingBox(0, 0, 1920, 87)

    def test_roi_passthrough_ignores_frame_content(self) -> None:
        det = RoiPassthroughDetector(100, 50)
        frame = Frame(timestamp_ms=0, image=Image.new("RGB", (320, 240), "red"))
        assert det.detect(frame).box == BoundingBox(0, 0, 100, 50)

    def test_roi_passthrough_rejects_non_positive(self) -> None:
        with pytest.raises(ValueError):
            RoiPassthroughDetector(0, 10)


# ---------------------------------------------------------------------------
# Pipeline 透传
# ---------------------------------------------------------------------------


class TestPipelineRoiPassthrough:
    def test_crop_region_passthrough_same_image_object(self) -> None:
        rec = PerformanceRecorder(mode=PerformanceMode.SUMMARY)
        pipeline = Pipeline(
            detector=RoiPassthroughDetector(320, 80),
            ocr=MockOcrEngine(text="t", confidence=0.9),
            config=Config(enable_line_select=False),
            performance_recorder=rec,
        )
        img = Image.new("RGB", (320, 80), "white")
        frame = Frame(timestamp_ms=0, image=img)
        out = pipeline._crop_to_region(frame, BoundingBox(0, 0, 320, 80))
        assert out is img
        payload = rec.to_payload()
        rec.close()
        assert payload["throughput"]["full_frame_passthrough_count"] == 1
        assert payload["throughput"]["pipeline_crop_count"] == 0

    def test_crop_region_non_full_still_crops(self) -> None:
        rec = PerformanceRecorder(mode=PerformanceMode.SUMMARY)
        pipeline = Pipeline(
            detector=FixedRegionDetector(BoundingBox(10, 20, 100, 40)),
            ocr=MockOcrEngine(text="t", confidence=0.9),
            config=Config(enable_line_select=False),
            performance_recorder=rec,
        )
        img = Image.new("RGB", (320, 240), "white")
        frame = Frame(timestamp_ms=0, image=img)
        out = pipeline._crop_to_region(frame, BoundingBox(10, 20, 100, 40))
        assert out is not img
        assert out.size == (100, 40)
        payload = rec.to_payload()
        rec.close()
        assert payload["throughput"]["pipeline_crop_count"] == 1
        assert payload["throughput"]["full_frame_passthrough_count"] == 0

    def test_roi_path_pipeline_crop_count_zero(self) -> None:
        rec = PerformanceRecorder(mode=PerformanceMode.SUMMARY)
        pipeline = Pipeline(
            detector=RoiPassthroughDetector(320, 80),
            ocr=MockOcrEngine(text="hello", confidence=0.99),
            config=Config(enable_line_select=False),
            performance_recorder=rec,
        )
        frames = [
            Frame(timestamp_ms=i * 200, image=Image.new("RGB", (320, 80), "gray")) for i in range(5)
        ]
        pipeline.run_frames(iter(frames))
        payload = rec.to_payload()
        rec.close()
        assert payload["throughput"]["pipeline_crop_count"] == 0
        assert payload["throughput"]["full_frame_passthrough_count"] >= 5

    def test_subtitle_profile_unchanged_under_roi(self) -> None:
        profile = SubtitleProfile.from_crop(1920, 87, script="cjk")
        assert profile.center_y == 43  # 相对 ROI，不是 source 848+
        pipeline = Pipeline(
            detector=RoiPassthroughDetector(1920, 87),
            ocr=MockOcrEngine(text="你好", confidence=0.95),
            config=Config(
                enable_line_select=True,
                subtitle_script="cjk",
                subtitle_profile=profile,
            ),
        )
        frame = Frame(timestamp_ms=0, image=Image.new("RGB", (1920, 87), "black"))
        pipeline.feed(frame)
        assert pipeline.subtitle_profile is not None
        assert pipeline.subtitle_profile.center_y == 43
        assert pipeline._region is not None
        assert pipeline._region.box == BoundingBox(0, 0, 1920, 87)


# ---------------------------------------------------------------------------
# Extractor 单元
# ---------------------------------------------------------------------------


class TestFrameIOPlan:
    def test_build_output_vf_full_and_roi_order(self) -> None:
        assert build_output_vf(2.0, None) == "fps=2.0"
        crop = BoundingBox(1, 2, 100, 50)
        vf = build_output_vf(5.0, crop)
        assert "format=rgb24" in vf
        assert vf.index("fps=") < vf.index("format=rgb24") < vf.index("crop=")
        assert "exact=1" in vf

    def test_plan_full_with_region_no_crop(self, tmp_path: Path) -> None:
        video = tmp_path / "v.mp4"
        _generate_solid_video(video, duration=1.0)
        plan = plan_frame_io(
            video,
            BoundingBox(0, 180, 320, 60),
            mode="full",
        )
        assert plan.output_mode == "full_rgb"
        assert plan.output_crop is None
        assert isinstance(plan.detector, FixedRegionDetector)

    def test_plan_roi_uses_passthrough(self, tmp_path: Path) -> None:
        video = tmp_path / "v.mp4"
        _generate_solid_video(video, duration=1.0)
        with patch(
            "sublift.extractor.frame_io.probe_source_frame",
            return_value=SourceFrameInfo(width=320, height=240, display_transform_ok=True),
        ):
            plan = plan_frame_io(
                video,
                BoundingBox(0, 180, 320, 60),
                mode="roi",
                on_unvalidated_transform="error",
            )
        assert plan.output_mode == "roi_rgb"
        assert plan.output_crop == BoundingBox(0, 180, 320, 60)
        assert isinstance(plan.detector, RoiPassthroughDetector)

    def test_plan_auto_falls_back_on_unvalidated_transform(self, tmp_path: Path) -> None:
        video = tmp_path / "v.mp4"
        _generate_solid_video(video, duration=1.0)
        with patch(
            "sublift.extractor.frame_io.probe_source_frame",
            return_value=SourceFrameInfo(
                width=320,
                height=240,
                display_transform_ok=False,
                transform_note="rotate=90",
            ),
        ):
            plan = plan_frame_io(
                video,
                BoundingBox(0, 180, 320, 60),
                mode="auto",
                on_unvalidated_transform="fallback_full",
            )
        assert plan.output_mode == "full_rgb"
        assert plan.output_crop is None
        assert isinstance(plan.detector, FixedRegionDetector)
        assert plan.fallback_reason is not None

    def test_plan_roi_errors_on_unvalidated_transform(self, tmp_path: Path) -> None:
        video = tmp_path / "v.mp4"
        with (
            patch(
                "sublift.extractor.frame_io.probe_source_frame",
                return_value=SourceFrameInfo(
                    width=320,
                    height=240,
                    display_transform_ok=False,
                    transform_note="rotate=90",
                ),
            ),
            pytest.raises(RuntimeError, match="显示变换"),
        ):
            plan_frame_io(
                video,
                BoundingBox(0, 180, 320, 60),
                mode="roi",
                on_unvalidated_transform="error",
            )

    def test_plan_no_region_uses_bottom_crop(self, tmp_path: Path) -> None:
        """无 region → 全帧 + BottomCrop，永不 ROI。"""
        video = tmp_path / "v.mp4"
        plan = plan_frame_io(video, None, mode="auto")
        assert plan.output_mode == "full_rgb"
        assert plan.output_crop is None
        assert isinstance(plan.detector, BottomCropDetector)
        assert plan.source is None

    def test_plan_roi_without_region_raises(self, tmp_path: Path) -> None:
        video = tmp_path / "v.mp4"
        with pytest.raises(ValueError, match="region_box"):
            plan_frame_io(video, None, mode="roi")

    def test_plan_invalid_mode_raises(self, tmp_path: Path) -> None:
        video = tmp_path / "v.mp4"
        with pytest.raises(ValueError, match="mode"):
            plan_frame_io(
                video,
                BoundingBox(0, 180, 320, 60),
                mode="jpeg",  # type: ignore[arg-type]
            )

    def test_plan_auto_with_region_uses_roi(self, tmp_path: Path) -> None:
        """GUI path mode 默认 auto + 有效 region → ROI。"""
        video = tmp_path / "v.mp4"
        region = BoundingBox(0, 180, 320, 60)
        with patch(
            "sublift.extractor.frame_io.probe_source_frame",
            return_value=SourceFrameInfo(width=320, height=240, display_transform_ok=True),
        ):
            plan = plan_frame_io(video, region, mode="auto")
        assert plan.output_mode == "roi_rgb"
        assert plan.output_crop == region
        assert isinstance(plan.detector, RoiPassthroughDetector)
        assert plan.fallback_reason is None


class TestSourceBoxOnRoiImageIsWrong:
    """负向：source-frame box 不得直接注入 ROI Frame（二次裁剪语义错误）。"""

    def test_fixed_region_source_box_on_roi_image_not_passthrough(self) -> None:
        # 典型 Zootopia 字幕带：source [0,848,1920,87]；ROI 图只有 87 高
        source_box = BoundingBox(0, 848, 1920, 87)
        roi_img = Image.new("RGB", (1920, 87), "red")
        frame = Frame(timestamp_ms=0, image=roi_img)
        wrong = FixedRegionDetector(source_box).detect(frame)
        assert wrong.box == source_box  # 仍吐 source 坐标

        pipeline = Pipeline(
            detector=FixedRegionDetector(source_box),
            ocr=MockOcrEngine(text="t", confidence=0.9),
            config=Config(enable_line_select=False),
        )
        out = pipeline._crop_to_region(frame, wrong.box)
        # 不是全幅 frame-local → 走 PIL crop，非零拷贝
        assert out is not roi_img
        # 越界 y 导致内容丢失（全黑/空），而非原 ROI 像素
        assert list(out.get_flattened_data()) != list(roi_img.get_flattened_data())

    def test_roi_passthrough_preserves_pixels(self) -> None:
        roi_img = Image.new("RGB", (1920, 87), "red")
        frame = Frame(timestamp_ms=0, image=roi_img)
        region = RoiPassthroughDetector(1920, 87).detect(frame)
        assert region.box == BoundingBox(0, 0, 1920, 87)
        pipeline = Pipeline(
            detector=RoiPassthroughDetector(1920, 87),
            ocr=MockOcrEngine(text="t", confidence=0.9),
            config=Config(enable_line_select=False),
        )
        out = pipeline._crop_to_region(frame, region.box)
        assert out is roi_img


class TestFfmpegExtractorRoiUnit:
    def test_output_crop_property(self) -> None:
        crop = BoundingBox(0, 180, 320, 60)
        ext = FfmpegExtractor(fps=1.0, output_crop=crop)
        assert ext.output_crop == crop

    def test_roi_filter_converts_to_rgb_before_crop(self) -> None:
        """filter 链必须在 crop 前 format=rgb24，避免 yuv 色度漂移。"""
        from sublift.extractor.frame_io import build_output_vf

        crop = BoundingBox(0, 180, 320, 60)
        vf = build_output_vf(5.0, crop)
        assert vf == "fps=5.0,format=rgb24,crop=320:60:0:180:exact=1"
        assert vf.index("format=rgb24") < vf.index("crop=")
        assert build_output_vf(1.0, None) == "fps=1.0"

    def test_output_crop_invalid_raises_before_spawn(self, tmp_path: Path) -> None:
        video = tmp_path / "tiny.mp4"
        _generate_solid_video(video, width=320, height=240, duration=1.0)
        ext = FfmpegExtractor(
            fps=1.0,
            output_crop=BoundingBox(0, 200, 320, 100),  # y+h=300 > 240
        )
        with pytest.raises(ValueError, match="越界") as ei:
            list(ext.extract(video))
        assert "source=320x240" in str(ei.value)


# ---------------------------------------------------------------------------
# 合成视频 integration
# ---------------------------------------------------------------------------


def _generate_solid_video(
    path: Path,
    *,
    width: int = 320,
    height: int = 240,
    duration: float = 2.0,
    fps: float = 2.0,
) -> None:
    """生成上下分色测试视频：上灰下红，便于 ROI 像素比对。"""
    # 使用 color + overlay 构造：底部 60px 红色
    # filter: color 灰底 + color 红条 vstack 更简单
    cmd = [
        "ffmpeg",
        "-v",
        "error",
        "-f",
        "lavfi",
        "-i",
        f"color=c=0x808080:s={width}x{height - 60}:r={fps}:d={duration}",
        "-f",
        "lavfi",
        "-i",
        f"color=c=0xFF0000:s={width}x60:r={fps}:d={duration}",
        "-filter_complex",
        "vstack=inputs=2",
        "-c:v",
        "libx264",
        "-pix_fmt",
        "yuv420p",
        str(path),
        "-y",
    ]
    subprocess.run(cmd, capture_output=True, check=True)


@pytest.mark.integration
class TestFfmpegExtractorRoiIntegration:
    def test_roi_frame_size_and_bytes(self, tmp_path: Path) -> None:
        video = tmp_path / "roi.mp4"
        _generate_solid_video(video)
        rec = PerformanceRecorder(mode=PerformanceMode.SUMMARY)
        crop = BoundingBox(0, 180, 320, 60)
        ext = FfmpegExtractor(fps=1.0, output_crop=crop, performance_recorder=rec)
        frames = list(ext.extract(video))
        payload = rec.to_payload()
        rec.close()

        assert len(frames) >= 1
        for f in frames:
            assert f.image.size == (320, 60)
            assert f.image.mode == "RGB"
        assert payload["throughput"]["output_mode"] == "roi_rgb"
        assert payload["throughput"]["raw_bytes_per_frame"] == 320 * 60 * 3
        assert payload["throughput"]["source_width"] == 320
        assert payload["throughput"]["source_height"] == 240
        assert payload["throughput"]["output_width"] == 320
        assert payload["throughput"]["output_height"] == 60
        assert payload["throughput"]["source_region_box"] == [0, 180, 320, 60]
        assert payload["throughput"]["raw_output_bytes"] == len(frames) * 320 * 60 * 3

    def test_roi_pixels_match_full_then_crop(self, tmp_path: Path) -> None:
        video = tmp_path / "roi.mp4"
        _generate_solid_video(video)
        crop = BoundingBox(0, 180, 320, 60)

        full_frames = list(FfmpegExtractor(fps=1.0).extract(video))
        roi_frames = list(FfmpegExtractor(fps=1.0, output_crop=crop).extract(video))
        assert len(full_frames) == len(roi_frames)
        for full, roi in zip(full_frames, roi_frames, strict=True):
            expected = full.image.crop((0, 180, 320, 240))
            assert roi.image.size == expected.size
            assert list(roi.image.get_flattened_data()) == list(expected.get_flattened_data())

    def test_roi_frame_count_and_timestamps_match_full(self, tmp_path: Path) -> None:
        video = tmp_path / "roi.mp4"
        _generate_solid_video(video, duration=3.0)
        crop = BoundingBox(0, 180, 320, 60)
        full_ts = [f.timestamp_ms for f in FfmpegExtractor(fps=1.0).extract(video)]
        roi_ts = [f.timestamp_ms for f in FfmpegExtractor(fps=1.0, output_crop=crop).extract(video)]
        assert full_ts == roi_ts
        assert full_ts == [0, 1000, 2000]

    def test_roi_odd_crop_exact(self, tmp_path: Path) -> None:
        video = tmp_path / "roi.mp4"
        _generate_solid_video(video)
        # 奇数坐标/尺寸；exact=1 应保持
        crop = BoundingBox(1, 181, 101, 51)
        frames = list(FfmpegExtractor(fps=1.0, output_crop=crop).extract(video))
        assert frames
        for f in frames:
            assert f.image.size == (101, 51)

    def test_roi_cancel_midway(self, tmp_path: Path) -> None:
        video = tmp_path / "roi.mp4"
        _generate_solid_video(video, duration=5.0)
        crop = BoundingBox(0, 180, 320, 60)
        ext = FfmpegExtractor(fps=1.0, output_crop=crop)
        frames = []
        for frame in ext.extract(video):
            frames.append(frame)
            if len(frames) == 2:
                ext.cancel()
        assert len(frames) == 2
        # 第二任务可启动
        ext2 = FfmpegExtractor(fps=1.0, output_crop=crop)
        more = list(ext2.extract(video))
        assert len(more) >= 2

    def test_full_path_unchanged_without_crop(self, tmp_path: Path) -> None:
        video = tmp_path / "full.mp4"
        _generate_solid_video(video)
        frames = list(FfmpegExtractor(fps=1.0).extract(video))
        assert frames
        for f in frames:
            assert f.image.size == (320, 240)


# ---------------------------------------------------------------------------
# Bridge / benchmark 路由（mock 级）
# ---------------------------------------------------------------------------


class TestBridgeRoiRouting:
    def test_path_mode_with_region_uses_roi_extractor_and_passthrough(self, tmp_path: Path) -> None:
        import asyncio

        from sublift.ipc.bridge import BridgeHandler
        from sublift.ipc.protocol import build_start_job
        from sublift.ocr.mock import MockOcrEngine

        video = tmp_path / "v.mp4"
        _generate_solid_video(video, duration=1.0)

        bridge = BridgeHandler(ocr_engine_factory=MockOcrEngine)
        pushed: list[dict[str, Any]] = []
        captured: dict[str, Any] = {}
        real_ctor = FfmpegExtractor
        detectors_seen: list[type[Any]] = []

        def capture_extractor(*args: Any, **kwargs: Any) -> FfmpegExtractor:
            captured["kwargs"] = kwargs
            # 构造时 pipeline 应已切换为 RoiPassthrough
            if bridge._pipeline is not None:
                detectors_seen.append(type(bridge._pipeline._detector))
            return real_ctor(*args, **kwargs)

        async def collect(msg: dict[str, Any]) -> None:
            pushed.append(msg)

        async def run() -> None:
            msg = build_start_job(
                "V1",
                1.0,
                "mock",
                0.5,
                duration_ms=1000,
                region_box=[0, 180, 320, 60],
                video_path=str(video),
            )
            with patch("sublift.ipc.bridge.FfmpegExtractor", side_effect=capture_extractor):
                await bridge.handle(msg, collect)
                if bridge._path_task is not None:
                    await bridge._path_task

        with patch(
            "sublift.extractor.frame_io.probe_source_frame",
            return_value=SourceFrameInfo(width=320, height=240, display_transform_ok=True),
        ):
            asyncio.run(run())

        crop = captured.get("kwargs", {}).get("output_crop")
        assert crop is not None
        assert crop == BoundingBox(0, 180, 320, 60)
        assert RoiPassthroughDetector in detectors_seen
        # plan 注入 source_info，避免二次 ffprobe
        assert captured.get("kwargs", {}).get("source_info") is not None
        types = [m.get("type") for m in pushed]
        assert "entries" in types or "done" in types
        assert not any(m.get("type") == "done" and m.get("ok") is False for m in pushed)

    def test_path_mode_invalid_region_errors(self, tmp_path: Path) -> None:
        import asyncio

        from sublift.ipc.bridge import BridgeHandler
        from sublift.ipc.protocol import build_start_job
        from sublift.ocr.mock import MockOcrEngine

        video = tmp_path / "v.mp4"
        _generate_solid_video(video, duration=1.0)
        bridge = BridgeHandler(ocr_engine_factory=MockOcrEngine)
        pushed: list[dict[str, Any]] = []

        async def collect(msg: dict[str, Any]) -> None:
            pushed.append(msg)

        async def run() -> None:
            msg = build_start_job(
                "V1",
                1.0,
                "mock",
                0.5,
                duration_ms=1000,
                region_box=[0, 200, 320, 100],
                video_path=str(video),
            )
            await bridge.handle(msg, collect)
            if bridge._path_task is not None:
                await bridge._path_task

        with patch(
            "sublift.extractor.frame_io.probe_source_frame",
            return_value=SourceFrameInfo(width=320, height=240, display_transform_ok=True),
        ):
            asyncio.run(run())

        done = [m for m in pushed if m.get("type") == "done"]
        assert done
        assert done[-1].get("ok") is False
        err = str(done[-1].get("error", ""))
        assert "source=320x240" in err
        assert "requested=" in err or "[0, 200, 320, 100]" in err

    def test_path_mode_unvalidated_transform_falls_back_full(self, tmp_path: Path) -> None:
        import asyncio

        from sublift.ipc.bridge import BridgeHandler
        from sublift.ipc.protocol import build_start_job
        from sublift.ocr.mock import MockOcrEngine

        video = tmp_path / "v.mp4"
        _generate_solid_video(video, duration=1.0)
        bridge = BridgeHandler(ocr_engine_factory=MockOcrEngine)
        captured: dict[str, Any] = {}
        real_ctor = FfmpegExtractor

        def capture_extractor(*args: Any, **kwargs: Any) -> FfmpegExtractor:
            captured["kwargs"] = kwargs
            return real_ctor(*args, **kwargs)

        pushed: list[dict[str, Any]] = []

        async def collect(msg: dict[str, Any]) -> None:
            pushed.append(msg)

        async def run() -> None:
            msg = build_start_job(
                "V1",
                1.0,
                "mock",
                0.5,
                duration_ms=1000,
                region_box=[0, 180, 320, 60],
                video_path=str(video),
            )
            with patch("sublift.ipc.bridge.FfmpegExtractor", side_effect=capture_extractor):
                await bridge.handle(msg, collect)
                if bridge._path_task is not None:
                    await bridge._path_task

        with patch(
            "sublift.extractor.frame_io.probe_source_frame",
            return_value=SourceFrameInfo(
                width=320,
                height=240,
                display_transform_ok=False,
                transform_note="stream.tags.rotate='90'",
            ),
        ):
            asyncio.run(run())

        assert captured.get("kwargs", {}).get("output_crop") is None
        assert not any(m.get("type") == "done" and m.get("ok") is False for m in pushed)

    def test_path_mode_without_region_full_bottom_crop(self, tmp_path: Path) -> None:
        """path mode 无 region → 全帧 + BottomCrop，不启用 ROI。"""
        import asyncio

        from sublift.ipc.bridge import BridgeHandler
        from sublift.ipc.protocol import build_start_job
        from sublift.ocr.mock import MockOcrEngine

        video = tmp_path / "v.mp4"
        _generate_solid_video(video, duration=1.0)
        bridge = BridgeHandler(ocr_engine_factory=MockOcrEngine)
        captured: dict[str, Any] = {}
        detectors_seen: list[type[Any]] = []
        real_ctor = FfmpegExtractor

        def capture_extractor(*args: Any, **kwargs: Any) -> FfmpegExtractor:
            captured["kwargs"] = kwargs
            if bridge._pipeline is not None:
                detectors_seen.append(type(bridge._pipeline._detector))
            return real_ctor(*args, **kwargs)

        pushed: list[dict[str, Any]] = []

        async def collect(msg: dict[str, Any]) -> None:
            pushed.append(msg)

        async def run() -> None:
            msg = build_start_job(
                "V1",
                1.0,
                "mock",
                0.5,
                duration_ms=1000,
                video_path=str(video),
            )
            with patch("sublift.ipc.bridge.FfmpegExtractor", side_effect=capture_extractor):
                await bridge.handle(msg, collect)
                if bridge._path_task is not None:
                    await bridge._path_task

        asyncio.run(run())

        assert captured.get("kwargs", {}).get("output_crop") is None
        assert BottomCropDetector in detectors_seen
        assert not any(m.get("type") == "done" and m.get("ok") is False for m in pushed)


class TestBenchmarkFrameOutputMode:
    def test_manifest_frame_output_mode_roi(self, tmp_path: Path) -> None:
        from sublift.benchmark.config import load_run_config

        repo = tmp_path / "repo"
        manifest_dir = repo / "benchmark" / "manifests"
        manifest_dir.mkdir(parents=True)
        (repo / "pyproject.toml").write_text("", encoding="utf-8")
        (repo / "feature-list.json").write_text("{}", encoding="utf-8")
        manifest = manifest_dir / "run.json"
        manifest.write_text(
            json.dumps(
                {
                    "video": "debug/movie.mp4",
                    "ground_truth": "benchmark/datasets/movie.srt",
                    "region_box": [0, 848, 1920, 87],
                    "frame_output_mode": "roi",
                }
            ),
            encoding="utf-8",
        )
        config = load_run_config(manifest)
        assert config.frame_output_mode == "roi"
        assert config.region_box == (0, 848, 1920, 87)

    def test_manifest_frame_output_mode_default_full(self, tmp_path: Path) -> None:
        from sublift.benchmark.config import load_run_config

        repo = tmp_path / "repo"
        manifest_dir = repo / "benchmark" / "manifests"
        manifest_dir.mkdir(parents=True)
        (repo / "pyproject.toml").write_text("", encoding="utf-8")
        (repo / "feature-list.json").write_text("{}", encoding="utf-8")
        manifest = manifest_dir / "run.json"
        manifest.write_text(
            json.dumps(
                {
                    "video": "debug/movie.mp4",
                    "ground_truth": "benchmark/datasets/movie.srt",
                    "region_box": [0, 848, 1920, 87],
                }
            ),
            encoding="utf-8",
        )
        config = load_run_config(manifest)
        assert config.frame_output_mode == "full"

    def test_manifest_roi_without_region_rejected(self, tmp_path: Path) -> None:
        from sublift.benchmark.config import ManifestError, load_run_config

        manifest = tmp_path / "run.json"
        manifest.write_text(
            json.dumps(
                {
                    "video": "debug/movie.mp4",
                    "ground_truth": "benchmark/datasets/movie.srt",
                    "frame_output_mode": "roi",
                }
            ),
            encoding="utf-8",
        )
        with pytest.raises(ManifestError, match="region_box"):
            load_run_config(manifest)

    def test_run_once_roi_records_output_metadata(self, tmp_path: Path) -> None:
        from sublift.benchmark.config import RunConfig
        from sublift.benchmark.runner import run_benchmark

        video = tmp_path / "v.mp4"
        _generate_solid_video(video, duration=1.0)
        gt = tmp_path / "gt.srt"
        gt.write_text(
            "1\n00:00:00,000 --> 00:00:01,000\nhello\n\n",
            encoding="utf-8",
        )
        config = RunConfig(
            video_path=video,
            ground_truth_path=gt,
            fps=1.0,
            engine="mock",
            region_box=(0, 180, 320, 60),
            frame_output_mode="roi",
            performance_mode="summary",
            warmup_runs=0,
            measured_runs=1,
            isolate_processes=False,
            output_dir=tmp_path / "out",
        )
        result = run_benchmark(config)
        assert result.performance is not None
        thr = result.performance["throughput"]
        assert thr["output_mode"] == "roi_rgb"
        assert thr["raw_bytes_per_frame"] == 320 * 60 * 3
        assert thr["source_region_box"] == [0, 180, 320, 60]
        assert thr["pipeline_crop_count"] == 0
        assert thr["full_frame_passthrough_count"] >= thr["frame_count"]
