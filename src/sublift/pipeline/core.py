"""端到端编排：detector → signature → changepoint → timeline → ocr → dedupe。

Pipeline 串联七个组件，支持两种使用模式：

1. **批量模式**（向后兼容）：``run()`` / ``run_frames()`` 一次性处理全部帧，
   返回完整字幕列表。
2. **流式模式**（feat-029）：``feed()`` 逐帧推进打轴，段闭合时返回
   :class:`SegmentEvent`；调用方按需 ``ocr_segment()``，最终 ``finalize()``
   跑全局 dedupe 返回最终列表。

流式模式让 bridge 边收帧边处理，首条字幕反馈时间从「全片处理完」降到
「第一段闭合 + 一次 OCR」，内存从「全部帧」降到「1 帧 + N 个 anchor」。
"""

from __future__ import annotations

from collections.abc import Iterator
from contextlib import nullcontext
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Any

import cv2
import numpy as np

from sublift.config import DEFAULT_CONFIG, Config
from sublift.detector.base import Detector
from sublift.extractor.base import Extractor
from sublift.models import (
    SCRIPT_CJK,
    BoundingBox,
    Frame,
    Region,
    SubtitleEntry,
    SubtitleProfile,
)
from sublift.ocr.base import OcrEngine
from sublift.pipeline.changepoint import ChangePointDetector, EventType
from sublift.pipeline.dedupe import merge_entries
from sublift.pipeline.line_select import (
    cjk_ratio,
    cleanup_subtitle_text,
    consensus_text,
    latin_ratio,
    script_score,
    select_line,
    should_accept_text,
)
from sublift.pipeline.signature import compute_signature
from sublift.pipeline.timeline import TimelineBuilder

if TYPE_CHECKING:
    from PIL import Image

    from sublift.diagnostics.performance import PerformanceRecorder
    from sublift.diagnostics.trace import TraceRecorder


@dataclass(frozen=True)
class SegmentEvent:
    """段闭合事件。

    由 :meth:`Pipeline.feed` 在段闭合（OUT/CHANGE）时返回，携带 OCR
    代表帧供 :meth:`Pipeline.ocr_segment` 使用。

    Attributes:
        start_ms: 段起始时间戳（毫秒）。
        end_ms: 段结束时间戳（毫秒）。
        anchor_frame: 主 OCR 帧（延迟锚或稳定帧）；None 时 OCR 空文本。
        fallback_frames: 额外候选帧（主帧失败时按序重试，feat-033b）。
    """

    start_ms: int
    end_ms: int
    anchor_frame: Frame | None
    fallback_frames: tuple[Frame, ...] = ()


class Pipeline:
    """字幕提取流水线编排器。

    组合 detector/ocr，内部串联 signature/changepoint/timeline/dedupe。

    两种使用模式：

    批量模式（向后兼容）::

        pipeline = Pipeline(detector, ocr, config, extractor=ext)
        entries = pipeline.run(video_path)

    流式模式（feat-029）::

        pipeline = Pipeline(detector, ocr, config)
        for frame in stream:
            event = pipeline.feed(frame)
            if event is not None:
                entry = pipeline.ocr_segment(event)
                # 增量推送 entry 给 UI
        final = pipeline.finalize()  # 关闭末段 + dedupe
    """

    def __init__(
        self,
        detector: Detector,
        ocr: OcrEngine,
        config: Config = DEFAULT_CONFIG,
        *,
        extractor: Extractor | None = None,
        trace_recorder: TraceRecorder | None = None,
        performance_recorder: PerformanceRecorder | None = None,
    ) -> None:
        """初始化流水线。

        Args:
            detector: 字幕区域检测器（首帧一次性确定 Region）。
            ocr: OCR 引擎（每段代表帧调用一次）。
            config: 提取流程配置。
            extractor: 帧采样器，文件模式必填（run() 调用）。
                帧流模式（run_frames()/feed()）不需要，可不传。
            trace_recorder: 可选打轴决策 trace 记录器（feat-031a）。
                注入后 ``ChangePointDetector`` 会逐帧记录决策上下文。
            performance_recorder: 可选性能记录器（feat-037）。
                None 时热点路径仅一次空值判断，不改变提取语义。
        """
        self._extractor = extractor
        self._detector = detector
        self._ocr = ocr
        self._config = config
        self._trace_recorder = trace_recorder
        self._perf = performance_recorder

        # 流式状态（feed/ocr_segment/finalize 共享）
        self._changepoint = ChangePointDetector(
            config=self._config.change_point,
            trace_recorder=self._trace_recorder,
        )
        self._timeline = TimelineBuilder()
        self._region: Region | None = None
        self._anchor_frames: dict[int, Frame] = {}
        self._closed_entries: list[SubtitleEntry] = []
        self._open_segment_start_ms: int | None = None
        self._processed_count = 0
        self._last_timestamp_ms = 0
        self._cancelled = False
        # feat-033b：OCR 锚帧延迟 + 稳定帧/首帧回退
        self._pending_anchor_remaining: int = 0
        self._segment_first_frame: Frame | None = None
        self._segment_stable_frame: Frame | None = None
        # feat-034b：字幕轨画像（显式传入或 Region 确定后由 crop 推导）
        self._subtitle_profile: SubtitleProfile | None = config.subtitle_profile
        # feat-034d：段内采样帧，供多帧共识
        self._segment_sample_frames: list[Frame] = []

    def _perf_span(self, stage: str, *, sample: bool = False) -> Any:
        """性能 span；无 recorder 时返回 nullcontext（一次空值判断）。"""
        perf = self._perf
        if perf is None:
            return nullcontext()
        return perf.span(stage, sample=sample)

    # ------------------------------------------------------------------
    # 流式 API（feat-029）
    # ------------------------------------------------------------------

    def feed(self, frame: Frame) -> SegmentEvent | None:
        """推进一帧打轴。

        轻量同步（<1ms）：仅做 crop + signature + changepoint。
        段闭合（OUT/CHANGE）时返回 :class:`SegmentEvent`，调用方应随后调用
        :meth:`ocr_segment` 获取该段 OCR 文本。

        Args:
            frame: 视频帧。

        Returns:
            段闭合事件；未闭合时返回 None。取消后返回 None 且不处理。
        """
        if self._cancelled:
            return None

        self._last_timestamp_ms = frame.timestamp_ms
        self._processed_count += 1

        # 首帧检测字幕区域
        if self._region is None:
            self._region = self._detector.detect(frame)
            if self._region is None:
                return None
            self._ensure_subtitle_profile()

        with self._perf_span("crop"):
            crop_image = self._crop_region(frame, self._region.box)
        with self._perf_span("color_convert"):
            crop_np = cv2.cvtColor(np.asarray(crop_image), cv2.COLOR_RGB2BGR)

        with self._perf_span("signature"):
            signature = compute_signature(
                crop_np, frame.timestamp_ms, self._config.signature
            )
        with self._perf_span("changepoint"):
            event = self._changepoint.process(signature, crop_np)

        if event is None:
            self._on_stable_frame(frame)
            return None

        if event.event_type == EventType.IN:
            self._open_segment(event.timestamp_ms, frame)
            self._timeline.consume(event)
            return None

        if event.event_type == EventType.OUT:
            start_ms = self._open_segment_start_ms
            self._timeline.consume(event)
            seg_event = self._close_segment(
                start_ms=start_ms if start_ms is not None else event.timestamp_ms,
                end_ms=event.timestamp_ms,
                closing_frame=frame,
            )
            return seg_event

        # CHANGE：旧段闭合 + 新段开启
        old_start = self._open_segment_start_ms
        self._timeline.consume(event)
        end_ms = event.prev_end_ms if event.prev_end_ms is not None else event.timestamp_ms
        # 闭合旧段时用稳定帧（本帧已是新字幕，勿作旧段 OCR 主锚）
        closed = self._close_segment(
            start_ms=old_start if old_start is not None else event.timestamp_ms,
            end_ms=end_ms,
            closing_frame=None,
        )
        self._open_segment(event.timestamp_ms, frame)
        return closed

    def ocr_segment(self, event: SegmentEvent) -> SubtitleEntry:
        """OCR 一个已闭合的段并缓存 raw entry。

        重操作（~几百 ms）：调用方应放到线程池，避免阻塞事件循环。
        OCR 完成后 raw entry 缓存到内部列表，供 :meth:`finalize` dedupe。

        feat-034：多代表帧行级选择 + 共识；低置信稳定中文可放行。
        未启用行级选择时回退旧逻辑（整区 join + 全局阈值）。

        Args:
            event: :meth:`feed` 返回的段闭合事件。

        Returns:
            带 OCR 文本的 SubtitleEntry（未经 dedupe）。
        """
        if self._region is None:
            entry = SubtitleEntry(
                start_ms=event.start_ms,
                end_ms=event.end_ms,
                text="",
                confidence=0.0,
            )
            self._closed_entries.append(entry)
            self._record_segment_perf(
                event,
                representative_frames=0,
                ocr_calls=0,
                ocr_wall_ns=0,
                select_wall_ns=0,
                accepted=False,
                output_chars=0,
            )
            return entry

        seg_stats = {"ocr_calls": 0, "ocr_ns": 0, "select_ns": 0, "rep_frames": 0}
        if self._config.enable_line_select:
            text, confidence = self._ocr_segment_with_line_select(event, seg_stats)
        else:
            text, confidence = self._ocr_segment_legacy(event, seg_stats)

        entry = SubtitleEntry(
            start_ms=event.start_ms,
            end_ms=event.end_ms,
            text=text,
            confidence=confidence,
        )
        self._closed_entries.append(entry)
        accepted = bool(text.strip())
        if self._perf is not None and accepted:
            self._perf.mark_first_entry()
        self._record_segment_perf(
            event,
            representative_frames=int(seg_stats["rep_frames"]),
            ocr_calls=int(seg_stats["ocr_calls"]),
            ocr_wall_ns=int(seg_stats["ocr_ns"]),
            select_wall_ns=int(seg_stats["select_ns"]),
            accepted=accepted,
            output_chars=len(text),
        )
        return entry

    def finalize(self) -> list[SubtitleEntry]:
        """关闭末尾未闭合段（如有），OCR 之，全局 dedupe 返回最终列表。

        Returns:
            去重合并后的字幕条目列表。
        """
        with self._perf_span("finalize"):
            # 关闭末尾未闭合段
            if self._open_segment_start_ms is not None:
                self._timeline.finalize_open_segment(self._last_timestamp_ms)
                segments = self._timeline.build()
                if segments:
                    last = segments[-1]
                    if last.end_ms is not None:
                        event = self._close_segment(
                            start_ms=last.start_ms,
                            end_ms=last.end_ms,
                            closing_frame=None,
                            clear_open=False,
                        )
                        self.ocr_segment(event)
                self._open_segment_start_ms = None
                self._reset_segment_ocr_state()

            with self._perf_span("dedupe"):
                result = merge_entries(
                    self._closed_entries,
                    merge_gap_ms=self._config.merge_gap_ms,
                    min_duration_ms=self._config.min_duration_ms,
                    drop_empty_text=self._config.drop_empty_text,
                )
        if self._perf is not None:
            self._perf.sample_resources()
        return result

    def cancel(self) -> None:
        """取消并清理内部状态，释放内存。后续 feed() 将拒绝处理。"""
        self._cancelled = True
        self._changepoint.reset()
        self._timeline.reset()
        self._region = None
        self._anchor_frames.clear()
        self._closed_entries.clear()
        self._open_segment_start_ms = None
        self._processed_count = 0
        self._last_timestamp_ms = 0
        self._reset_segment_ocr_state()
        self._subtitle_profile = self._config.subtitle_profile

    @property
    def subtitle_profile(self) -> SubtitleProfile | None:
        """当前生效的字幕轨画像（feat-034b）；行级选择在 034c 消费。"""
        return self._subtitle_profile

    def _ensure_subtitle_profile(self) -> None:
        """Region 就绪且无显式 profile 时，用 crop 全带推导默认画像。"""
        if self._subtitle_profile is not None or self._region is None:
            return
        box = self._region.box
        self._subtitle_profile = SubtitleProfile.from_crop(
            box.width,
            box.height,
            script=self._config.subtitle_script,
        )

    def _open_segment(self, start_ms: int, frame: Frame) -> None:
        """开启新段并启动 OCR 锚帧延迟（feat-033b）。"""
        self._open_segment_start_ms = start_ms
        self._segment_first_frame = frame
        self._segment_stable_frame = None
        self._segment_sample_frames = [frame]
        delay = max(0, self._config.ocr_anchor_delay_frames)
        if delay == 0:
            self._anchor_frames[start_ms] = frame
            self._pending_anchor_remaining = 0
        else:
            self._pending_anchor_remaining = delay

    def _on_stable_frame(self, frame: Frame) -> None:
        """无状态事件时：推进延迟锚 / 更新稳定帧 / 采样共识帧。"""
        if self._open_segment_start_ms is None:
            return
        if self._pending_anchor_remaining > 0:
            self._pending_anchor_remaining -= 1
            if self._pending_anchor_remaining == 0:
                self._anchor_frames[self._open_segment_start_ms] = frame
            self._record_sample_frame(frame)
            return
        self._segment_stable_frame = frame
        self._record_sample_frame(frame)

    def _record_sample_frame(self, frame: Frame) -> None:
        """段内保留最多 ocr_consensus_frames 个采样（首帧 + 中间稀疏 + 最新）。"""
        cap = max(1, self._config.ocr_consensus_frames)
        samples = self._segment_sample_frames
        if not samples:
            samples.append(frame)
            return
        if samples[-1].timestamp_ms == frame.timestamp_ms:
            samples[-1] = frame
            return
        if len(samples) < cap:
            samples.append(frame)
            return
        # 已满：保留首帧，用新帧替换末帧；若 cap>=3 再偶尔替换中位
        samples[-1] = frame
        if cap >= 3 and len(samples) >= 3:
            # 每累计到末帧时把上一「次新」挤到中间槽，保持时间跨度
            mid = len(samples) // 2
            if samples[mid].timestamp_ms < frame.timestamp_ms:
                # 把中位向右挪的简易策略：中位取 (first, last) 中点已在列表中的较新者
                samples[mid] = samples[-2] if len(samples) > 2 else samples[mid]

    def _close_segment(
        self,
        *,
        start_ms: int,
        end_ms: int,
        closing_frame: Frame | None,
        clear_open: bool = True,
    ) -> SegmentEvent:
        """选取 OCR 主锚与回退帧，并清理段状态。"""
        delayed = self._anchor_frames.pop(start_ms, None)
        first = self._segment_first_frame
        stable = self._segment_stable_frame
        # 主锚：延迟锁定帧 > 稳定帧 > 段首帧
        anchor = delayed or stable or first
        # 回退候选：采样帧 + 稳定/段首/闭合/延迟（去重）
        fallbacks: list[Frame] = []
        seen: set[int] = set()
        if anchor is not None:
            seen.add(anchor.timestamp_ms)
        ordered: list[Frame | None] = list(self._segment_sample_frames)
        ordered.extend([stable, first, closing_frame, delayed])
        for cand in ordered:
            if cand is None or cand.timestamp_ms in seen:
                continue
            seen.add(cand.timestamp_ms)
            fallbacks.append(cand)

        if clear_open:
            self._open_segment_start_ms = None
            self._reset_segment_ocr_state()

        return SegmentEvent(
            start_ms=start_ms,
            end_ms=end_ms,
            anchor_frame=anchor,
            fallback_frames=tuple(fallbacks),
        )

    def _reset_segment_ocr_state(self) -> None:
        self._pending_anchor_remaining = 0
        self._segment_first_frame = None
        self._segment_stable_frame = None
        self._segment_sample_frames = []

    def _ocr_segment_legacy(
        self,
        event: SegmentEvent,
        seg_stats: dict[str, int] | None = None,
    ) -> tuple[str, float]:
        """旧路径：整区 join + 全局阈值 + 单锚回退。"""
        text, confidence = self._ocr_frame_raw(event.anchor_frame, seg_stats)
        if self._needs_ocr_retry(text, confidence):
            seen_ts = {
                event.anchor_frame.timestamp_ms
                if event.anchor_frame is not None
                else -1
            }
            for fb in event.fallback_frames:
                if fb.timestamp_ms in seen_ts:
                    continue
                seen_ts.add(fb.timestamp_ms)
                fb_text, fb_conf = self._ocr_frame_raw(fb, seg_stats)
                if not self._needs_ocr_retry(fb_text, fb_conf):
                    text, confidence = fb_text, fb_conf
                    break
                if fb_text.strip() and not text.strip():
                    text, confidence = fb_text, fb_conf
                if fb_conf > confidence and fb_text.strip():
                    text, confidence = fb_text, fb_conf

        if confidence < self._config.confidence_threshold:
            text = ""
        return text, confidence

    def _ocr_segment_with_line_select(
        self,
        event: SegmentEvent,
        seg_stats: dict[str, int] | None = None,
    ) -> tuple[str, float]:
        """行级选择 + 多帧共识（feat-034c/d）。"""
        self._ensure_subtitle_profile()
        profile = self._subtitle_profile
        if profile is None:
            # 无 profile 时退化为 crop 默认
            if self._region is not None:
                box = self._region.box
                profile = SubtitleProfile.from_crop(
                    box.width,
                    box.height,
                    script=self._config.subtitle_script,
                )
                self._subtitle_profile = profile
            else:
                return "", 0.0

        frames = self._collect_ocr_frames(event)
        if seg_stats is not None:
            seg_stats["rep_frames"] = len(frames)
        samples: list[tuple[str, float]] = []

        for frame in frames:
            text, conf = self._ocr_frame_selected(frame, profile, seg_stats)
            if not text.strip():
                continue
            samples.append((text, conf))

            with self._perf_span("consensus"):
                t0 = self._perf.now_ns() if self._perf is not None else 0
                partial = consensus_text(samples, script=profile.script)
                if seg_stats is not None and self._perf is not None:
                    seg_stats["select_ns"] += self._perf.now_ns() - t0

            # 高置信且不含混合文字系统：单帧即可，少做 OCR。CJK 与拉丁
            # 粘连时继续取样，让段内稳定性决定是合法混排还是横幅水印。
            mixed_script = cjk_ratio(text) > 0.0 and latin_ratio(text) > 0.0
            if (
                conf >= self._config.confidence_threshold
                and script_score(text, profile.script)
                >= self._config.line_select_min_script
                and not (profile.script == SCRIPT_CJK and mixed_script)
            ):
                break
            # 相似变体已形成 ≥2 票共识即可提前结束，不要求全文精确相等。
            if (
                partial.support_votes >= 2
                and partial.confidence >= self._config.low_conf_threshold
            ):
                break

        with self._perf_span("consensus"):
            t0 = self._perf.now_ns() if self._perf is not None else 0
            consensus = consensus_text(samples, script=profile.script)
            if seg_stats is not None and self._perf is not None:
                seg_stats["select_ns"] += self._perf.now_ns() - t0
        if not consensus.text:
            return "", 0.0

        with self._perf_span("cleanup"):
            t0 = self._perf.now_ns() if self._perf is not None else 0
            text = cleanup_subtitle_text(consensus.text, profile.script)
            if seg_stats is not None and self._perf is not None:
                seg_stats["select_ns"] += self._perf.now_ns() - t0
        confidence = consensus.confidence
        if not text:
            return "", confidence

        accept = should_accept_text(
            text,
            confidence,
            profile=profile,
            confidence_threshold=self._config.confidence_threshold,
            low_conf_threshold=self._config.low_conf_threshold,
            support_votes=consensus.support_votes,
        )
        if not accept:
            return "", confidence
        return text, confidence

    def _collect_ocr_frames(self, event: SegmentEvent) -> list[Frame]:
        """主锚 + fallback，截断到 ocr_consensus_frames。"""
        cap = max(1, self._config.ocr_consensus_frames)
        frames: list[Frame] = []
        seen: set[int] = set()
        for fr in (event.anchor_frame, *event.fallback_frames):
            if fr is None or fr.timestamp_ms in seen:
                continue
            seen.add(fr.timestamp_ms)
            frames.append(fr)
            if len(frames) >= cap:
                break
        return frames

    def _ocr_frame_selected(
        self,
        frame: Frame | None,
        profile: SubtitleProfile,
        seg_stats: dict[str, int] | None = None,
    ) -> tuple[str, float]:
        """单帧 OCR → 行级选择 → (text, conf)。"""
        if frame is None or self._region is None:
            return "", 0.0
        with self._perf_span("crop"):
            crop_image = self._crop_region(frame, self._region.box)
        with self._perf_span("ocr", sample=True):
            t0 = self._perf.now_ns() if self._perf is not None else 0
            result = self._ocr.recognize(crop_image)
            if seg_stats is not None and self._perf is not None:
                seg_stats["ocr_calls"] += 1
                seg_stats["ocr_ns"] += self._perf.now_ns() - t0
            elif seg_stats is not None:
                seg_stats["ocr_calls"] += 1

        if result.lines:
            with self._perf_span("line_select"):
                t0 = self._perf.now_ns() if self._perf is not None else 0
                chosen = select_line(
                    result.lines,
                    profile,
                    min_score=self._config.line_select_min_score,
                    min_script=self._config.line_select_min_script,
                )
                if seg_stats is not None and self._perf is not None:
                    seg_stats["select_ns"] += self._perf.now_ns() - t0
            if chosen is None:
                return "", 0.0
            with self._perf_span("cleanup"):
                t0 = self._perf.now_ns() if self._perf is not None else 0
                text = cleanup_subtitle_text(chosen.text, profile.script)
                if seg_stats is not None and self._perf is not None:
                    seg_stats["select_ns"] += self._perf.now_ns() - t0
            return text, chosen.confidence

        # 无 lines 时兼容旧引擎输出
        with self._perf_span("cleanup"):
            t0 = self._perf.now_ns() if self._perf is not None else 0
            text = cleanup_subtitle_text(result.text, profile.script)
            if seg_stats is not None and self._perf is not None:
                seg_stats["select_ns"] += self._perf.now_ns() - t0
        return text, result.confidence

    def _ocr_frame_raw(
        self,
        frame: Frame | None,
        seg_stats: dict[str, int] | None = None,
    ) -> tuple[str, float]:
        if frame is None or self._region is None:
            return "", 0.0
        with self._perf_span("crop"):
            crop_image = self._crop_region(frame, self._region.box)
        with self._perf_span("ocr", sample=True):
            t0 = self._perf.now_ns() if self._perf is not None else 0
            result = self._ocr.recognize(crop_image)
            if seg_stats is not None and self._perf is not None:
                seg_stats["ocr_calls"] += 1
                seg_stats["ocr_ns"] += self._perf.now_ns() - t0
            elif seg_stats is not None:
                seg_stats["ocr_calls"] += 1
        if seg_stats is not None and seg_stats.get("rep_frames", 0) == 0:
            seg_stats["rep_frames"] = 1
        return result.text, result.confidence

    def _record_segment_perf(
        self,
        event: SegmentEvent,
        *,
        representative_frames: int,
        ocr_calls: int,
        ocr_wall_ns: int,
        select_wall_ns: int,
        accepted: bool,
        output_chars: int,
    ) -> None:
        perf = self._perf
        if perf is None:
            return
        perf.record_segment(
            start_ms=event.start_ms,
            end_ms=event.end_ms,
            representative_frames=representative_frames,
            ocr_calls=ocr_calls,
            ocr_wall_ns=ocr_wall_ns,
            select_wall_ns=select_wall_ns,
            accepted=accepted,
            output_chars=output_chars,
        )

    def _ocr_frame(self, frame: Frame | None) -> tuple[str, float]:
        """兼容旧调用：按配置选择路径。"""
        if self._config.enable_line_select:
            self._ensure_subtitle_profile()
            profile = self._subtitle_profile
            if profile is not None:
                return self._ocr_frame_selected(frame, profile)
        return self._ocr_frame_raw(frame)

    def _needs_ocr_retry(self, text: str, confidence: float) -> bool:
        if not text.strip():
            return True
        return confidence < self._config.confidence_threshold

    @property
    def processed_count(self) -> int:
        """已处理帧数。"""
        return self._processed_count

    # ------------------------------------------------------------------
    # 批量 API（向后兼容）
    # ------------------------------------------------------------------

    def run(self, video_path: Path) -> list[SubtitleEntry]:
        """执行端到端字幕提取（文件模式）。

        Args:
            video_path: 视频文件路径。

        Returns:
            清理后的字幕条目列表。

        Raises:
            RuntimeError: 未传 extractor。
        """
        if self._extractor is None:
            raise RuntimeError("文件模式 run() 需要 extractor，请在构造 Pipeline 时传入")
        if self._perf is not None:
            self._perf.mark_core_start()
        try:
            frames = self._extractor.extract(video_path)
            return self.run_frames(frames, _mark_core=False)
        finally:
            if self._perf is not None:
                self._perf.mark_core_end()
                self._perf.sample_resources()

    def run_frames(
        self,
        frames: Iterator[Frame],
        *,
        _mark_core: bool = True,
    ) -> list[SubtitleEntry]:
        """执行端到端字幕提取（帧流模式，批量向后兼容）。

        内部走 feed + ocr_segment + finalize 流式路径，结果与旧实现等价。

        Args:
            frames: 帧迭代器。
            _mark_core: 是否标记 core wall（``run()`` 已标记时传 False）。

        Returns:
            清理后的字幕条目列表。
        """
        if _mark_core and self._perf is not None:
            self._perf.mark_core_start()
        self._reset_streaming_state()
        try:
            for frame in frames:
                event = self.feed(frame)
                if event is not None:
                    self.ocr_segment(event)
            return self.finalize()
        finally:
            if _mark_core and self._perf is not None:
                self._perf.mark_core_end()
                self._perf.sample_resources()

    # ------------------------------------------------------------------
    # 内部辅助
    # ------------------------------------------------------------------

    def _reset_streaming_state(self) -> None:
        """重置流式状态（feed/ocr_segment/finalize 共享）。"""
        self._changepoint = ChangePointDetector(
            config=self._config.change_point,
            trace_recorder=self._trace_recorder,
        )
        self._timeline = TimelineBuilder()
        self._region = None
        self._anchor_frames = {}
        self._closed_entries = []
        self._open_segment_start_ms = None
        self._processed_count = 0
        self._last_timestamp_ms = 0
        self._cancelled = False
        self._subtitle_profile = self._config.subtitle_profile
        self._reset_segment_ocr_state()

    @staticmethod
    def _crop_region(frame: Frame, box: BoundingBox) -> Image.Image:
        """从帧裁剪字幕区域。"""
        return frame.image.crop(
            (box.x, box.y, box.x + box.width, box.y + box.height)
        )
