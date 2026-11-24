#!/usr/bin/env python3
"""Pure in-memory & mock unit tests for tools.vlm_ocr (no network, no ffmpeg, no video files)."""

from __future__ import annotations

import io
import json
import queue
import random
import re
import sys
import tempfile
import threading
import unittest
import urllib.error
from http.client import HTTPMessage
from pathlib import Path
from unittest import mock

from PIL import Image

PROJECT_ROOT = Path(__file__).resolve().parent.parent
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

from tools.vlm_ocr.assembler import (  # noqa: E402
    assemble_cues_from_slots,
    extract_json_object,
    format_srt,
    normalize_subtitle_line,
    validate_and_align_sheet_slots,
    validate_payload_against_schema,
)
from tools.vlm_ocr.cli import PlaceholderSlotFallbackResolver  # noqa: E402
from tools.vlm_ocr.env_config import (  # noqa: E402
    normalize_chat_completions_url,
    parse_dotenv,
    resolve_vlm_config,
)
from tools.vlm_ocr.models import (  # noqa: E402
    PLACEHOLDER_CATEGORY,
    PLACEHOLDER_TEXT,
    CropBox,
    FailedSlotRecord,
    SheetSpec,
    TimingSlot,
    TranscribedSlot,
    VlmEndpointConfig,
)
from tools.vlm_ocr.sheet_builder import (  # noqa: E402
    InMemoryStripExtractor,
    PreparedSheet,
    build_contact_sheets,
    compose_contact_sheet_image,
    compute_strip_crop_geometry,
    compute_target_strip_height,
    encode_image_png_bytes,
)
from tools.vlm_ocr.timing_adapter import (  # noqa: E402
    ProgressiveSegmentMerger,
    VideoStreamMeta,
    _build_timing_cli_cmd,
    _normalize_skip_frame,
    _run_segmented_timing_stream,
    compute_bottom_fallback_box,
    compute_segment_windows,
    load_timing_slots,
    merge_segment_timing_events,
    parse_auto_region_from_cli_output,
    resolve_segment_hwaccel,
    resolve_timing_parallelism,
    run_timing_stage,
    stream_timing_stage,
)
from tools.vlm_ocr.vlm_client import (  # noqa: E402
    _RateLimitGate,
    summarize_vlm_usage,
    transcribe_contact_sheets,
    transcribe_streaming_sheets,
)


class _FakeHttpResponse:
    def __init__(self, payload: dict[str, object]) -> None:
        self._body = json.dumps(payload).encode("utf-8")

    def read(self) -> bytes:
        return self._body

    def __enter__(self) -> _FakeHttpResponse:
        return self

    def __exit__(self, *_args: object) -> None:
        return None


def _make_http_400(url: str, message: str) -> urllib.error.HTTPError:
    body = json.dumps({"type": "error", "error": {"message": message}}).encode("utf-8")
    return urllib.error.HTTPError(
        url=url,
        code=400,
        msg="Bad Request",
        hdrs=None,  # type: ignore[arg-type]
        fp=io.BytesIO(body),
    )


def _make_http_429(url: str, message: str, retry_after: str = "0") -> urllib.error.HTTPError:
    body = json.dumps({"type": "error", "error": {"message": message}}).encode("utf-8")
    hdrs = HTTPMessage()
    hdrs["Retry-After"] = retry_after
    return urllib.error.HTTPError(
        url=url,
        code=429,
        msg="Too Many Requests",
        hdrs=hdrs,
        fp=io.BytesIO(body),
    )


class TestEnvConfig(unittest.TestCase):
    def test_dotenv_resolution_priority_and_secret_redaction(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            env_path = Path(tmp) / ".env"
            env_path.write_text(
                "\n".join(
                    [
                        "# comment",
                        "MINIMAX_API_KEY='secret-from-dotenv'",
                        "MINIMAX_API_BASE_URL=https://api.minimax.cn/v1",
                        "MINIMAX_MODEL=MiniMax-M3.1-Flash",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            parsed = parse_dotenv(env_path)
            self.assertEqual(parsed["MINIMAX_API_KEY"], "secret-from-dotenv")

            cfg = resolve_vlm_config(env_file=env_path, environ={})
            self.assertEqual(cfg.api_key, "secret-from-dotenv")
            self.assertEqual(cfg.endpoint, "https://api.minimax.cn/v1/chat/completions")
            self.assertEqual(cfg.model, "MiniMax-M3.1-Flash")
            self.assertNotIn("secret-from-dotenv", repr(cfg))

            # Process env overrides .env, CLI overrides process env
            cfg_env = resolve_vlm_config(
                env_file=env_path,
                environ={"MINIMAX_API_KEY": "secret-from-env"},
            )
            self.assertEqual(cfg_env.api_key, "secret-from-env")

            cfg_cli = resolve_vlm_config(
                api_key="secret-from-cli",
                endpoint="https://custom.example/v1/chat/completions",
                env_file=env_path,
                environ={"MINIMAX_API_KEY": "secret-from-env"},
            )
            self.assertEqual(cfg_cli.api_key, "secret-from-cli")
            self.assertEqual(cfg_cli.endpoint, "https://custom.example/v1/chat/completions")

    def test_normalize_chat_completions_url(self) -> None:
        self.assertEqual(
            normalize_chat_completions_url("https://api.minimax.cn/v1/"),
            "https://api.minimax.cn/v1/chat/completions",
        )
        self.assertEqual(
            normalize_chat_completions_url("https://api.minimax.cn/v1/chat/completions"),
            "https://api.minimax.cn/v1/chat/completions",
        )

    def test_nvidia_provider_and_auto_profile_selection(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            env_both = Path(tmp) / ".env.both"
            env_both.write_text(
                "\n".join(
                    [
                        "MINIMAX_API_KEY='mm-secret'",
                        "MINIMAX_API_BASE_URL=https://api.minimax.cn/v1",
                        "MINIMAX_MODEL=MiniMax-M3.1-Flash",
                        "NVIDIA_API_KEY='nvapi-secret'",
                        "NVIDIA_API_BASE_URL=https://integrate.api.nvidia.com/v1",
                        "NVIDIA_MODEL=nvidia/nemotron-3.5-super-vl-120b-a12b",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            # Both keys present: auto preserves MiniMax default, explicit provider/model picks NVIDIA
            cfg_auto = resolve_vlm_config(env_file=env_both, environ={})
            self.assertEqual(cfg_auto.api_key, "mm-secret")
            self.assertEqual(cfg_auto.model, "MiniMax-M3.1-Flash")
            self.assertEqual(cfg_auto.endpoint, "https://api.minimax.cn/v1/chat/completions")

            cfg_nv = resolve_vlm_config(provider="nvidia", env_file=env_both, environ={})
            self.assertEqual(cfg_nv.api_key, "nvapi-secret")
            self.assertEqual(cfg_nv.model, "nvidia/nemotron-3.5-super-vl-120b-a12b")
            self.assertEqual(cfg_nv.endpoint, "https://integrate.api.nvidia.com/v1/chat/completions")
            self.assertNotIn("nvapi-secret", repr(cfg_nv))

            cfg_model_hint = resolve_vlm_config(
                model="nvidia/nemotron-3.5-super-vl-120b-a12b",
                env_file=env_both,
                environ={},
            )
            self.assertEqual(cfg_model_hint.api_key, "nvapi-secret")
            self.assertEqual(
                cfg_model_hint.endpoint,
                "https://integrate.api.nvidia.com/v1/chat/completions",
            )

            # Empty MINIMAX_API_KEY with template URL/MODEL present must switch cleanly to NVIDIA without cross-pairing
            env_nv_only = Path(tmp) / ".env.nvidia_only"
            env_nv_only.write_text(
                "\n".join(
                    [
                        "MINIMAX_API_KEY=",
                        "MINIMAX_API_BASE_URL=https://api.minimax.cn/v1",
                        "MINIMAX_MODEL=MiniMax-M3.1-Flash",
                        "NVIDIA_API_KEY='nvapi-only-key'",
                        "NVIDIA_API_BASE_URL=https://integrate.api.nvidia.com/v1",
                        "NVIDIA_MODEL=nvidia/nemotron-3.5-super-vl-120b-a12b",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            cfg_nv_only = resolve_vlm_config(env_file=env_nv_only, environ={})
            self.assertEqual(cfg_nv_only.api_key, "nvapi-only-key")
            self.assertEqual(cfg_nv_only.model, "nvidia/nemotron-3.5-super-vl-120b-a12b")
            self.assertEqual(
                cfg_nv_only.endpoint,
                "https://integrate.api.nvidia.com/v1/chat/completions",
            )


class TestTimingAdapterAndSheetBuilder(unittest.TestCase):
    def test_parse_cli_output_and_load_jsonl_and_srt(self) -> None:
        stdout = (
            "正在执行自动字幕区域前置探测...\n"
            "自动区域探测成功: 区域=[x=0, y=778, w=1920, h=121], 置信度=0.95, 样例=\"你好\"\n"
        )
        box = parse_auto_region_from_cli_output(stdout)
        self.assertEqual(box, CropBox(0, 778, 1920, 121))
        self.assertEqual(compute_bottom_fallback_box(1920, 1080, 0.3), CropBox(0, 756, 1920, 324))

        with tempfile.TemporaryDirectory() as tmp:
            jsonl = Path(tmp) / "timing.jsonl"
            jsonl.write_text(
                '{"index":1,"start_ms":1000,"end_ms":2500}\n'
                '{"index":2,"start_ms":3000,"end_ms":3000}\n'
                '{"index":3,"start_ms":3200,"end_ms":4800}\n',
                encoding="utf-8",
            )
            slots = load_timing_slots(jsonl)
            self.assertEqual(len(slots), 2)
            self.assertEqual(slots[0].slot_id, 1)
            self.assertEqual(slots[0].mid_ms, 1750)
            self.assertEqual(slots[1].slot_id, 2)
            self.assertEqual(slots[1].mid_ms, 4000)

            srt = Path(tmp) / "timing.srt"
            srt.write_text(
                "1\n00:00:01,000 --> 00:00:02,000\nTIMING_ONLY\n\n",
                encoding="utf-8",
            )
            srt_slots = load_timing_slots(srt)
            self.assertEqual(len(srt_slots), 1)
            self.assertEqual((srt_slots[0].start_ms, srt_slots[0].end_ms), (1000, 2000))

    def test_strip_crop_geometry_and_contact_sheet_rendering(self) -> None:
        # Full-width strip trims outer 10% margins and adds 12px vertical pad
        full_crop = compute_strip_crop_geometry(1920, 1080, CropBox(0, 778, 1920, 121))
        self.assertEqual(full_crop, CropBox(192, 766, 1536, 145))
        self.assertGreaterEqual(compute_target_strip_height(full_crop, strip_w=960), 90)

        # Near-bottom box clamps safely to frame bounds
        clamped = compute_strip_crop_geometry(1920, 1080, CropBox(200, 1000, 800, 75))
        self.assertEqual(clamped, CropBox(200, 988, 800, 92))

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            strip_dir = root / "strips"
            sheet_dir = root / "sheets"
            strip_dir.mkdir()
            slots: list[TimingSlot] = []
            strip_paths: dict[int, Path] = {}
            for i in range(1, 4):
                slots.append(TimingSlot(i, i, i * 1000, i * 1000 + 800, i * 1000 + 400, 800))
                p = strip_dir / f"slot_{i:03d}.png"
                Image.new("RGB", (320, 40), (i * 40, 20, 20)).save(p)
                strip_paths[i] = p

            manifest = build_contact_sheets(
                slots,
                strip_paths,
                sheet_dir,
                slots_per_sheet=2,
                strip_w=960,
                strip_h=90,
                quiet=True,
            )
            self.assertEqual(len(manifest), 2)
            self.assertEqual(manifest[0].expected_slot_ids, [1, 2])
            self.assertEqual(manifest[1].expected_slot_ids, [3])
            self.assertTrue((sheet_dir / "sheets_manifest.json").is_file())
            with Image.open(manifest[0].sheet_path) as img:
                self.assertGreater(img.width, 960)
                self.assertGreater(img.height, 180)


class TestAssembler(unittest.TestCase):
    def test_extract_json_strips_think_and_fences(self) -> None:
        raw = (
            "<think>先仔细看看每一行</think>\n"
            "```json\n"
            '{"slots": [{"id": 1, "category": "dialogue", "lines": ["你好"]}]}\n'
            "```\n"
        )
        obj = extract_json_object(raw)
        self.assertEqual(obj["slots"][0]["id"], 1)

    def test_schema_rejects_bool_id_and_unknown_keys(self) -> None:
        with self.assertRaises(ValueError):
            validate_payload_against_schema(
                {"slots": [{"id": True, "category": "dialogue", "lines": ["x"]}]}
            )
        with self.assertRaises(ValueError):
            validate_payload_against_schema(
                {
                    "slots": [{"id": 1, "category": "dialogue", "lines": ["x"]}],
                    "extra": 1,
                }
            )
        with self.assertRaises(ValueError):
            validate_payload_against_schema(
                {"slots": [{"id": 1, "category": "bogus", "lines": ["x"]}]}
            )

    def test_cjk_vs_latin_normalization_and_internal_spaces(self) -> None:
        self.assertEqual(normalize_subtitle_line("哦  对?!", script="auto"), "哦 对？！")
        self.assertEqual(normalize_subtitle_line("( 前情提要... )", script="cjk"), "（前情提要…）")
        # Pure English line keeps half-width punctuation
        self.assertEqual(normalize_subtitle_line("Wait, what?!", script="auto"), "Wait, what?!")

    def test_local_id_remapping_and_blocked_merge_barrier(self) -> None:
        validated, remapped = validate_and_align_sheet_slots(
            {
                "slots": [
                    {"id": 1, "category": "dialogue", "lines": ["同一句"]},
                    {"id": 2, "category": "empty", "lines": ["should-be-cleared"]},
                    {"id": 3, "category": "dialogue", "lines": ["同一句"]},
                    {"id": 4, "category": "dialogue", "lines": ["同一句"]},
                ]
            },
            expected_slot_ids=[41, 42, 43, 44],
        )
        self.assertTrue(remapped)
        self.assertEqual([s.slot_id for s in validated], [41, 42, 43, 44])
        self.assertEqual(validated[1].lines, [])

        timing_slots = [
            TimingSlot(41, 1, 1000, 1500, 1250, 500),
            TimingSlot(42, 2, 1550, 1650, 1600, 100),  # empty slot -> barrier!
            TimingSlot(43, 3, 1700, 2200, 1950, 500),
            TimingSlot(44, 4, 2200, 2800, 2500, 600),
        ]
        cues, filtered, merged_groups = assemble_cues_from_slots(
            timing_slots, validated, merge_adjacent=True, max_merge_gap_ms=200
        )
        # #41 and #43 cannot merge across empty #42; #43 and #44 DO merge!
        self.assertEqual(len(cues), 2)
        self.assertEqual((cues[0].start_ms, cues[0].end_ms), (1000, 1500))
        self.assertEqual((cues[1].start_ms, cues[1].end_ms), (1700, 2800))
        self.assertEqual(len(filtered), 1)
        self.assertEqual(len(merged_groups), 1)
        srt_text = format_srt(cues)
        self.assertIn("00:00:01,000 --> 00:00:01,500", srt_text)
        self.assertIn("00:00:01,700 --> 00:00:02,800", srt_text)


class TestVlmClientMocked(unittest.TestCase):
    def _build_sheets(
        self, root: Path, count: int
    ) -> tuple[list[TimingSlot], dict[int, Path], list[SheetSpec]]:
        strip_dir = root / "strips"
        sheet_dir = root / "sheets"
        strip_dir.mkdir(parents=True, exist_ok=True)
        slots: list[TimingSlot] = []
        strip_paths: dict[int, Path] = {}
        for i in range(1, count + 1):
            slots.append(TimingSlot(i, i, i * 1000, i * 1000 + 800, i * 1000 + 400, 800))
            p = strip_dir / f"slot_{i:03d}.png"
            Image.new("RGB", (320, 40), (30, 30, 30)).save(p)
            strip_paths[i] = p
        manifest = build_contact_sheets(
            slots, strip_paths, sheet_dir, slots_per_sheet=4, strip_w=320, strip_h=40, quiet=True
        )
        return slots, strip_paths, manifest

    def test_adaptive_reasoning_fallback_and_split_rescue_and_resume(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            slots, strip_paths, manifest = self._build_sheets(root, 4)
            slots_by_id = {s.slot_id: s for s in slots}
            cfg = VlmEndpointConfig(
                endpoint="https://mock.endpoint/v1/chat/completions",
                api_key="test-key",
                model="generic-vlm",
                thinking_mode="none",
                max_retries=2,
            )

            seen_efforts: list[str | None] = []

            def fake_urlopen(req: object, timeout: float = 120.0) -> _FakeHttpResponse:  # noqa: ARG001
                body = json.loads(req.data.decode("utf-8"))  # type: ignore[attr-defined]
                eff = body.get("reasoning_effort")
                seen_efforts.append(eff)
                if eff == "none":
                    raise _make_http_400(
                        cfg.endpoint,
                        "invalid params, model requires adaptive thinking; reasoning effort none is not allowed (2013)",
                    )
                text_parts = [
                    p["text"]
                    for p in body["messages"][1]["content"]
                    if p.get("type") == "text"
                ]
                prompt_text = text_parts[0]
                m_ids = re.search(r"\[([0-9,\s]+)\]", prompt_text)
                assert m_ids is not None
                ids = json.loads(f"[{m_ids.group(1)}]")
                # Force slot count mismatch when batch size == 4 to trigger Sheet-Split Rescue!
                if len(ids) == 4:
                    resp_slots = [{"id": ids[0], "category": "dialogue", "lines": ["缺了三槽"]}]
                else:
                    resp_slots = [
                        {"id": sid, "category": "dialogue", "lines": [f"台词{sid}"]}
                        for sid in ids
                    ]
                return _FakeHttpResponse(
                    {
                        "choices": [{"message": {"content": json.dumps({"slots": resp_slots})}}],
                        "usage": {"prompt_tokens": 100, "completion_tokens": 20, "total_tokens": 120},
                    }
                )

            with (
                mock.patch("urllib.request.urlopen", side_effect=fake_urlopen),
                mock.patch("time.sleep", return_value=None),
            ):
                doc, transcribed = transcribe_contact_sheets(
                    manifest,
                    root,
                    config=cfg,
                    workers=1,
                    slots_by_id=slots_by_id,
                    strip_paths_by_id=strip_paths,
                    strip_w=320,
                    strip_h=40,
                    reuse_cache=False,
                    quiet=True,
                )

            # Should downgrade none -> low and cache it, then succeed via split-rescue
            self.assertEqual(seen_efforts[0], "none")
            self.assertTrue(all(e == "low" for e in seen_efforts[1:]))
            self.assertTrue(doc["batches"][0]["split_rescued"])
            self.assertEqual([s.slot_id for s in transcribed], [1, 2, 3, 4])
            self.assertEqual(transcribed[2].lines, ["台词3"])
            self.assertTrue((root / "vlm_responses.json").is_file())
            self.assertFalse((root / "vlm_responses.partial.json").exists())

    def test_partial_checkpoint_resume_skips_completed_sheets(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            slots, strip_paths, manifest = self._build_sheets(root, 6)  # 2 sheets: [1..4], [5..6]
            partial_file = root / "vlm_responses.partial.json"
            partial_file.write_text(
                json.dumps(
                    {
                        "model": "MiniMax-M3.1-Flash",
                        "batches": [
                            {
                                "sheet": manifest[0].sheet_name,
                                "expected_slot_ids": [1, 2, 3, 4],
                                "elapsed_s": 0.5,
                                "slots": [
                                    {"id": i, "category": "dialogue", "lines": [f"已断点{i}"]}
                                    for i in range(1, 5)
                                ],
                            }
                        ],
                    }
                ),
                encoding="utf-8",
            )

            called_sheets: list[str] = []

            def fake_urlopen(req: object, timeout: float = 120.0) -> _FakeHttpResponse:  # noqa: ARG001
                body = json.loads(req.data.decode("utf-8"))  # type: ignore[attr-defined]
                prompt_text = body["messages"][1]["content"][1]["text"]
                called_sheets.append(prompt_text)
                return _FakeHttpResponse(
                    {
                        "choices": [
                            {
                                "message": {
                                    "content": json.dumps(
                                        {
                                            "slots": [
                                                {"id": 5, "category": "dialogue", "lines": ["新请求5"]},
                                                {"id": 6, "category": "empty", "lines": []},
                                            ]
                                        }
                                    )
                                }
                            }
                        ],
                        "usage": {"prompt_tokens": 50, "completion_tokens": 10, "total_tokens": 60},
                    }
                )

            cfg = VlmEndpointConfig(
                endpoint="https://api.minimax.cn/v1/chat/completions",
                api_key="test-key",
                model="MiniMax-M3.1-Flash",
                thinking_mode="auto",
            )
            with mock.patch("urllib.request.urlopen", side_effect=fake_urlopen):
                _doc, transcribed = transcribe_contact_sheets(
                    manifest,
                    root,
                    config=cfg,
                    workers=1,
                    reuse_cache=True,
                    quiet=True,
                )

            # Only sheet_02 should be requested
            self.assertEqual(len(called_sheets), 1)
            self.assertIn("sheet_02.png", called_sheets[0])
            self.assertEqual(transcribed[0].lines, ["已断点1"])
            self.assertEqual(transcribed[4].lines, ["新请求5"])
            self.assertEqual(transcribed[5].category, "empty")


class _FakePipeStdout:
    def __init__(self, lines: list[str]) -> None:
        self._lines = list(lines)
        self.pos = 0
        self.closed = False

    def readline(self) -> str:
        if self.pos >= len(self._lines):
            return ""
        line = self._lines[self.pos]
        self.pos += 1
        return line

    def close(self) -> None:
        self.closed = True


class _FakePopenProc:
    def __init__(self, lines: list[str], returncode: int = 0) -> None:
        self.stdout = _FakePipeStdout(lines)
        self.returncode = returncode
        self._done = False

    def poll(self) -> int | None:
        return self.returncode if self._done else None

    def wait(self, timeout: float | None = None) -> int:  # noqa: ARG002
        self._done = True
        return self.returncode

    def terminate(self) -> None:
        self._done = True

    def kill(self) -> None:
        self._done = True


class TestStreamingVlmPipeline(unittest.TestCase):
    def test_stream_timing_stage_emits_header_and_slots_live(self) -> None:
        fake_lines = [
            "正在执行自动字幕区域前置探测...\n",
            '自动区域探测成功: 区域=[x=0, y=778, w=1920, h=121], 置信度=0.95, 样例="你好"\n',
            "开始处理视频: demo.mp4\n",
            "[字幕 #1] 00:00:01,000 --> 00:00:02,000 |  (置信度: 0.00)\n",
            "[字幕 #2] 00:00:03,000 --> 00:00:04,500 |  (置信度: 0.00)\n",
            "[字幕 #3] 00:00:05,000 --> 00:00:06,200 |  (置信度: 0.00)\n",
        ]
        fake_proc = _FakePopenProc(fake_lines)

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            video = root / "demo.mp4"
            video.write_bytes(b"")
            work_dir = root / "work"
            cli_bin = root / "sublift_cli"
            cli_bin.write_bytes(b"")

            with (
                mock.patch(
                    "tools.vlm_ocr.timing_adapter.probe_video_stream_info",
                    return_value=(1920, 1080, 10000),
                ),
                mock.patch("subprocess.Popen", return_value=fake_proc),
            ):
                header, slot_stream, meta = stream_timing_stage(
                    video,
                    work_dir,
                    cli_bin=cli_bin,
                    fps=5.0,
                    auto_region=True,
                    reuse_cache=False,
                    quiet=True,
                )
                # Region header must be resolved eagerly BEFORE consuming any cue lines.
                self.assertEqual(header.resolved_region, CropBox(0, 778, 1920, 121))
                self.assertTrue(header.auto_region_detected)
                self.assertEqual(header.source, "sublift_cli_stream")
                self.assertEqual(fake_proc.stdout.pos, 2)

                first = next(slot_stream)
                self.assertEqual((first.slot_id, first.start_ms, first.end_ms, first.mid_ms), (1, 1000, 2000, 1500))
                rest = list(slot_stream)
                self.assertEqual(len(rest), 2)
                self.assertEqual((rest[0].slot_id, rest[0].mid_ms), (2, 3750))
                self.assertEqual((rest[1].slot_id, rest[1].mid_ms), (3, 5600))

            self.assertEqual(meta["slot_count"], 3)
            self.assertTrue((work_dir / "timing_meta.json").is_file())
            self.assertTrue((work_dir / "sublift_timing.log").is_file())
            self.assertTrue(fake_proc.stdout.closed)

    def test_in_memory_sheet_composition_and_in_memory_split_rescue(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            work_dir = Path(tmp)
            slots = [
                TimingSlot(i, i, i * 1000, i * 1000 + 800, i * 1000 + 400, 800)
                for i in range(1, 5)
            ]

            def fake_prepare(sheet_idx: int, batch_slots: list[TimingSlot]) -> PreparedSheet:
                strips = {
                    s.slot_id: Image.new("RGB", (320, 40), (s.slot_id * 30, 40, 50))
                    for s in batch_slots
                }
                canvas = compose_contact_sheet_image(
                    batch_slots, strips, sheet_index=sheet_idx, strip_w=320, strip_h=40
                )
                png_bytes = encode_image_png_bytes(canvas, compress_level=1)
                return PreparedSheet(
                    spec=SheetSpec(
                        sheet_index=sheet_idx,
                        sheet_name=f"sheet_{sheet_idx:02d}.png",
                        sheet_path=f"memory://sheet_{sheet_idx:02d}.png",
                        expected_slot_ids=[s.slot_id for s in batch_slots],
                    ),
                    png_bytes=png_bytes,
                    batch_slots=list(batch_slots),
                    strip_images=strips,
                    decode_s=0.001,
                    encode_s=0.001,
                )

            cfg = VlmEndpointConfig(
                endpoint="https://mock.endpoint/v1/chat/completions",
                api_key="test-key",
                model="generic-vlm",
                thinking_mode="default",
                max_retries=1,
            )

            seen_batch_sizes: list[int] = []

            def fake_urlopen(req: object, timeout: float = 120.0) -> _FakeHttpResponse:  # noqa: ARG001
                body = json.loads(req.data.decode("utf-8"))  # type: ignore[attr-defined]
                prompt_text = body["messages"][1]["content"][1]["text"]
                m_ids = re.search(r"\[([0-9,\s]+)\]", prompt_text)
                assert m_ids is not None
                ids = json.loads(f"[{m_ids.group(1)}]")
                seen_batch_sizes.append(len(ids))
                if len(ids) == 4:
                    resp_slots = [{"id": ids[0], "category": "dialogue", "lines": ["仅返回一条"]}]
                else:
                    resp_slots = [
                        {"id": sid, "category": "dialogue", "lines": [f"内存台词{sid}"]}
                        for sid in ids
                    ]
                return _FakeHttpResponse(
                    {
                        "choices": [{"message": {"content": json.dumps({"slots": resp_slots})}}],
                        "usage": {"prompt_tokens": 50, "completion_tokens": 10, "total_tokens": 60},
                    }
                )

            with (
                mock.patch("urllib.request.urlopen", side_effect=fake_urlopen),
                mock.patch("time.sleep", return_value=None),
            ):
                doc, transcribed, collected_slots, stats = transcribe_streaming_sheets(
                    iter(slots),
                    fake_prepare,
                    work_dir,
                    config=cfg,
                    slots_per_sheet=4,
                    strip_w=320,
                    strip_h=40,
                    extract_workers=2,
                    vlm_workers=2,
                    save_debug_sheets=False,
                    reuse_cache=False,
                    quiet=True,
                )

            self.assertEqual(seen_batch_sizes[0], 4)
            self.assertEqual(sorted(seen_batch_sizes[1:]), [2, 2])
            self.assertTrue(doc["batches"][0]["split_rescued"])
            self.assertEqual([s.slot_id for s in transcribed], [1, 2, 3, 4])
            self.assertEqual([s.slot_id for s in collected_slots], [1, 2, 3, 4])
            self.assertEqual(transcribed[3].lines, ["内存台词4"])
            self.assertEqual(stats["in_memory_sheets_encoded"], 1)
            self.assertGreater(stats["total_sheet_png_bytes"], 0)
            # Strict zero temp PNG writes on disk in default streaming mode
            self.assertEqual(list(work_dir.rglob("*.png")), [])

    def test_streaming_checkpoint_skips_frame_decode_and_releases_buffers(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            work_dir = Path(tmp)
            slots = [
                TimingSlot(i, i, i * 1000, i * 1000 + 800, i * 1000 + 400, 800)
                for i in range(1, 7)
            ]
            (work_dir / "vlm_responses.partial.json").write_text(
                json.dumps(
                    {
                        "model": "MiniMax-M3.1-Flash",
                        "batches": [
                            {
                                "sheet": "sheet_01.png",
                                "expected_slot_ids": [1, 2, 3, 4],
                                "elapsed_s": 0.2,
                                "slots": [
                                    {"id": i, "category": "dialogue", "lines": [f"断点流{i}"]}
                                    for i in range(1, 5)
                                ],
                            }
                        ],
                    }
                ),
                encoding="utf-8",
            )

            prepared_seen: list[PreparedSheet] = []

            def fake_prepare(sheet_idx: int, batch_slots: list[TimingSlot]) -> PreparedSheet:
                strips = {
                    s.slot_id: Image.new("RGB", (320, 40), (10, 20, 30))
                    for s in batch_slots
                }
                canvas = compose_contact_sheet_image(
                    batch_slots, strips, sheet_index=sheet_idx, strip_w=320, strip_h=40
                )
                prep = PreparedSheet(
                    spec=SheetSpec(
                        sheet_index=sheet_idx,
                        sheet_name=f"sheet_{sheet_idx:02d}.png",
                        sheet_path=f"memory://sheet_{sheet_idx:02d}.png",
                        expected_slot_ids=[s.slot_id for s in batch_slots],
                    ),
                    png_bytes=encode_image_png_bytes(canvas, compress_level=1),
                    batch_slots=list(batch_slots),
                    strip_images=strips,
                    decode_s=0.001,
                    encode_s=0.001,
                )
                prepared_seen.append(prep)
                return prep

            def fake_urlopen(req: object, timeout: float = 120.0) -> _FakeHttpResponse:  # noqa: ARG001
                body = json.loads(req.data.decode("utf-8"))  # type: ignore[attr-defined]
                prompt_text = body["messages"][1]["content"][1]["text"]
                self.assertIn("sheet_02.png", prompt_text)
                return _FakeHttpResponse(
                    {
                        "choices": [
                            {
                                "message": {
                                    "content": json.dumps(
                                        {
                                            "slots": [
                                                {"id": 5, "category": "dialogue", "lines": ["流新5"]},
                                                {"id": 6, "category": "empty", "lines": []},
                                            ]
                                        }
                                    )
                                }
                            }
                        ],
                        "usage": {"prompt_tokens": 40, "completion_tokens": 10, "total_tokens": 50},
                    }
                )

            cfg = VlmEndpointConfig(
                endpoint="https://api.minimax.cn/v1/chat/completions",
                api_key="test-key",
                model="MiniMax-M3.1-Flash",
                thinking_mode="auto",
            )
            with mock.patch("urllib.request.urlopen", side_effect=fake_urlopen):
                _doc, transcribed, _all_slots, stats = transcribe_streaming_sheets(
                    iter(slots),
                    fake_prepare,
                    work_dir,
                    config=cfg,
                    slots_per_sheet=4,
                    strip_w=320,
                    strip_h=40,
                    extract_workers=2,
                    vlm_workers=2,
                    reuse_cache=True,
                    quiet=True,
                )

            # sheet_01 hit checkpoint without decoding; only sheet_02 was prepared!
            self.assertEqual(len(prepared_seen), 1)
            self.assertEqual(prepared_seen[0].spec.sheet_name, "sheet_02.png")
            # Strip & PNG buffers freed immediately after VLM call!
            self.assertEqual(prepared_seen[0].strip_images, {})
            self.assertEqual(prepared_seen[0].png_bytes, b"")
            self.assertEqual(stats["cached_sheet_hits"], 1)
            self.assertEqual(stats["in_memory_sheets_encoded"], 1)
            self.assertEqual(transcribed[0].lines, ["断点流1"])
            self.assertEqual(transcribed[4].lines, ["流新5"])
            self.assertEqual(transcribed[5].category, "empty")

    def test_rate_limit_429_retries_via_shared_gate_without_splitting(self) -> None:
        fake_now = [100.0]
        gate = _RateLimitGate(monotonic_fn=lambda: fake_now[0], jitter_fn=lambda: 0.25)
        delay0 = gate.record_429(retry_after_s=None, attempt=0)
        self.assertAlmostEqual(delay0, 1.75, places=3)
        delay_ra = gate.record_429(retry_after_s=3.0, attempt=1)
        self.assertAlmostEqual(delay_ra, 3.0, places=3)
        gate.record_success()
        self.assertEqual(gate._consecutive_429, 0)

        with tempfile.TemporaryDirectory() as tmp:
            work_dir = Path(tmp)
            slots = [
                TimingSlot(i, i, i * 1000, i * 1000 + 800, i * 1000 + 400, 800)
                for i in range(1, 5)
            ]
            strip_paths: dict[int, Path] = {}
            for s in slots:
                p = work_dir / f"strip_{s.slot_id}.png"
                Image.new("RGB", (320, 40), (12, 34, 56)).save(p)
                strip_paths[s.slot_id] = p

            manifest = build_contact_sheets(
                slots, strip_paths, work_dir / "sheets", slots_per_sheet=4, strip_w=320, strip_h=40, quiet=True
            )
            calls: list[list[int]] = []

            def fake_urlopen(req: object, timeout: float = 0.0) -> _FakeHttpResponse:
                del timeout
                body = json.loads(req.data.decode("utf-8"))  # type: ignore[attr-defined]
                prompt_text = body["messages"][1]["content"][1]["text"]
                match = re.search(r"期望槽位 id 列表依次为 (\[[^\]]+\])", prompt_text)
                req_ids = json.loads(match.group(1)) if match else []
                calls.append(req_ids)
                if len(calls) <= 2:
                    raise _make_http_429("https://api.minimax.cn/v1/chat/completions", "rate limit", retry_after="0")
                return _FakeHttpResponse(
                    {
                        "choices": [
                            {
                                "finish_reason": "stop",
                                "message": {
                                    "content": json.dumps(
                                        {
                                            "slots": [
                                                {"id": sid, "category": "dialogue", "lines": [f"限流恢复{sid}"]}
                                                for sid in req_ids
                                            ]
                                        }
                                    )
                                },
                            }
                        ],
                        "usage": {"prompt_tokens": 50, "completion_tokens": 20, "total_tokens": 70},
                    }
                )

            cfg = VlmEndpointConfig(
                endpoint="https://api.minimax.cn/v1/chat/completions",
                api_key="test-key",
                model="MiniMax-M3.1-Flash",
                thinking_mode="auto",
                max_retries=1,
            )
            with mock.patch("urllib.request.urlopen", side_effect=fake_urlopen):
                doc, transcribed = transcribe_contact_sheets(
                    manifest,
                    work_dir,
                    config=cfg,
                    workers=1,
                    slots_by_id={s.slot_id: s for s in slots},
                    strip_paths_by_id=strip_paths,
                    strip_w=320,
                    strip_h=40,
                    reuse_cache=False,
                    quiet=True,
                )

            self.assertEqual(len(calls), 3)
            self.assertEqual(calls, [[1, 2, 3, 4], [1, 2, 3, 4], [1, 2, 3, 4]])
            self.assertFalse(doc["batches"][0].get("split_rescued", False))
            self.assertEqual([t.text for t in transcribed], ["限流恢复1", "限流恢复2", "限流恢复3", "限流恢复4"])

    def test_content_filter_immediate_4_level_bisect_and_leaf_fallback_and_cache_bypass(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            work_dir = Path(tmp)
            slots = [
                TimingSlot(i, i, i * 1000, i * 1000 + 800, i * 1000 + 400, 800)
                for i in range(1, 11)
            ]

            def fake_prepare(sheet_idx: int, batch_slots: list[TimingSlot]) -> PreparedSheet:
                imgs = {s.slot_id: Image.new("RGB", (320, 40), (s.slot_id * 10, 40, 80)) for s in batch_slots}
                canvas = compose_contact_sheet_image(batch_slots, imgs, sheet_index=sheet_idx, strip_w=320, strip_h=40)
                png = encode_image_png_bytes(canvas, compress_level=1)
                spec = SheetSpec(
                    sheet_index=sheet_idx,
                    sheet_name=f"sheet_{sheet_idx:02d}.png",
                    sheet_path=f"memory://sheet_{sheet_idx:02d}.png",
                    expected_slot_ids=[s.slot_id for s in batch_slots],
                )
                return PreparedSheet(
                    spec=spec,
                    png_bytes=png,
                    batch_slots=list(batch_slots),
                    strip_images=imgs,
                    decode_s=0.01,
                    encode_s=0.01,
                )

            filter_active = [True]
            requested_batches: list[list[int]] = []

            def fake_urlopen(req: object, timeout: float = 0.0) -> _FakeHttpResponse:
                del timeout
                body = json.loads(req.data.decode("utf-8"))  # type: ignore[attr-defined]
                prompt_text = body["messages"][1]["content"][1]["text"]
                match = re.search(r"期望槽位 id 列表依次为 (\[[^\]]+\])", prompt_text)
                req_ids = json.loads(match.group(1)) if match else []
                requested_batches.append(req_ids)
                if filter_active[0] and 7 in req_ids:
                    return _FakeHttpResponse(
                        {
                            "choices": [
                                {
                                    "finish_reason": "content_filter",
                                    "message": {"role": "assistant", "content": ""},
                                }
                            ]
                        }
                    )
                return _FakeHttpResponse(
                    {
                        "choices": [
                            {
                                "finish_reason": "stop",
                                "message": {
                                    "content": json.dumps(
                                        {
                                            "slots": [
                                                {"id": sid, "category": "dialogue", "lines": [f"台词{sid}"]}
                                                for sid in req_ids
                                            ]
                                        }
                                    )
                                },
                            }
                        ],
                        "usage": {"prompt_tokens": 20, "completion_tokens": 10, "total_tokens": 30},
                    }
                )

            cfg = VlmEndpointConfig(
                endpoint="https://api.minimax.cn/v1/chat/completions",
                api_key="test-key",
                model="MiniMax-M3.1-Flash",
                thinking_mode="auto",
                max_retries=2,
            )
            with (
                mock.patch("urllib.request.urlopen", side_effect=fake_urlopen),
                mock.patch("tools.vlm_ocr.vlm_client.time.sleep") as mock_sleep,
            ):
                vlm_doc, transcribed, _slots_out, stats = transcribe_streaming_sheets(
                    iter(slots),
                    fake_prepare,
                    work_dir,
                    config=cfg,
                    slots_per_sheet=10,
                    strip_w=320,
                    strip_h=40,
                    extract_workers=1,
                    vlm_workers=1,
                    fallback_resolver=PlaceholderSlotFallbackResolver(),
                    reuse_cache=True,
                    quiet=True,
                )

            # Content filter must bisect immediately down to 1-slot leaf #7 without retry sleeps
            self.assertEqual(mock_sleep.call_count, 0)
            self.assertIn([7], requested_batches)
            self.assertEqual(stats["degraded_slots"], 1)
            self.assertEqual(summarize_vlm_usage(vlm_doc)["degraded_slots"], 1)
            self.assertTrue((work_dir / "raw_responses" / "sheet_01.json").is_file())
            failed_doc = json.loads((work_dir / "failed_slots.json").read_text(encoding="utf-8"))
            self.assertEqual(failed_doc["failed_count"], 1)
            rec = FailedSlotRecord(**failed_doc["slots"][0])
            self.assertEqual(rec.slot_id, 7)

            by_id = {t.slot_id: t for t in transcribed}
            self.assertEqual(by_id[7].category, PLACEHOLDER_CATEGORY)
            self.assertEqual(by_id[7].lines, [PLACEHOLDER_TEXT])
            self.assertEqual(by_id[6].lines, ["台词6"])
            self.assertEqual(by_id[8].lines, ["台词8"])

            # Adjacent [待补录] slots must NOT merge into one cue even with gap=0
            adj_slots = [
                TimingSlot(7, 7, 7000, 7800, 7400, 800),
                TimingSlot(8, 8, 7850, 8600, 8225, 750),
            ]
            adj_transcribed = [
                TranscribedSlot(7, PLACEHOLDER_CATEGORY, [PLACEHOLDER_TEXT]),
                TranscribedSlot(8, PLACEHOLDER_CATEGORY, [PLACEHOLDER_TEXT]),
            ]
            adj_cues, _noise, merged_groups = assemble_cues_from_slots(
                adj_slots, adj_transcribed, merge_adjacent=True, max_merge_gap_ms=500
            )
            self.assertEqual(len(adj_cues), 2)
            self.assertEqual(merged_groups, [])

            # Re-run with reuse_cache=True once upstream filter clears: degraded sheet MUST NOT hit cache
            filter_active[0] = False
            requested_batches.clear()
            with mock.patch("urllib.request.urlopen", side_effect=fake_urlopen):
                vlm_doc2, transcribed2, _slots2, stats2 = transcribe_streaming_sheets(
                    iter(slots),
                    fake_prepare,
                    work_dir,
                    config=cfg,
                    slots_per_sheet=10,
                    strip_w=320,
                    strip_h=40,
                    extract_workers=1,
                    vlm_workers=1,
                    fallback_resolver=PlaceholderSlotFallbackResolver(),
                    reuse_cache=True,
                    quiet=True,
                )
            self.assertEqual(stats2["cached_sheet_hits"], 0)
            self.assertEqual(stats2["degraded_slots"], 0)
            self.assertEqual(summarize_vlm_usage(vlm_doc2)["degraded_slots"], 0)
            self.assertEqual([t.text for t in transcribed2][6], "台词7")
            self.assertFalse((work_dir / "failed_slots.json").exists())

    def test_segment_windows_and_boundary_overlap_stitching(self) -> None:
        windows = compute_segment_windows(120_000, 50.0, overlap_ms=500)
        self.assertEqual(
            windows,
            [
                (0, 50_000, 0, 50_500),
                (50_000, 100_000, 49_500, 100_500),
                (100_000, 120_000, 99_500, 120_000),
            ],
        )

        stitched = merge_segment_timing_events(
            [
                (
                    0,
                    50_000,
                    [(1000, 3000), (49_200, 50_500), (50_100, 51_500)],
                ),
                (
                    50_000,
                    100_000,
                    [(49_000, 49_400), (49_500, 51_100), (60_000, 62_000)],
                ),
            ],
            overlap_ms=500,
        )
        self.assertEqual(
            [(s.slot_id, s.start_ms, s.end_ms) for s in stitched],
            [
                (1, 1000, 3000),
                (2, 49_200, 51_100),
                (3, 60_000, 62_000),
            ],
        )

        hd_meta = VideoStreamMeta(width=1920, height=1080, duration_ms=3_800_000, fps=24.0, codec_name="h264", pix_fmt="yuv420p")
        uhd_meta = VideoStreamMeta(width=3840, height=2160, duration_ms=2_400_000, fps=24.0, codec_name="hevc", pix_fmt="yuv420p10le")
        self.assertEqual(resolve_timing_parallelism(hd_meta, requested_workers=0, hwaccel="auto"), (1, 0.0))
        self.assertEqual(resolve_timing_parallelism(uhd_meta, requested_workers=0, hwaccel="auto"), (1, 0.0))

    def test_skip_frame_cli_cmd_and_timing_cache_key(self) -> None:
        self.assertEqual(_normalize_skip_frame(None), "auto")
        self.assertEqual(_normalize_skip_frame("nonref"), "noref")
        self.assertEqual(_normalize_skip_frame("off"), "none")
        with self.assertRaises(ValueError):
            _normalize_skip_frame("bad_mode")

        cmd_default = _build_timing_cli_cmd(
            cli_bin=Path("/usr/bin/true"),
            video_path=Path("/tmp/v.mp4"),
            out_jsonl=Path("/tmp/out.jsonl"),
            fps=5.0,
            bottom_ratio=0.3,
        )
        self.assertIn("--skip-frame", cmd_default)
        self.assertEqual(cmd_default[cmd_default.index("--skip-frame") + 1], "auto")

        cmd_nonref = _build_timing_cli_cmd(
            cli_bin=Path("/usr/bin/true"),
            video_path=Path("/tmp/v.mp4"),
            out_jsonl=Path("/tmp/out.jsonl"),
            fps=5.0,
            bottom_ratio=0.3,
            skip_frame="nonref",
        )
        self.assertEqual(cmd_nonref[cmd_nonref.index("--skip-frame") + 1], "noref")

        with tempfile.TemporaryDirectory() as tmp:
            work_dir = Path(tmp)
            video_path = work_dir / "fake.mp4"
            video_path.write_bytes(b"")
            cli_bin = work_dir / "sublift_cli"
            cli_bin.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")

            timing_jsonl = work_dir / "timing.jsonl"
            meta_path = work_dir / "timing_meta.json"
            timing_jsonl.write_text('{"index":1,"start_ms":1000,"end_ms":2000}\n', encoding="utf-8")
            # Legacy timing_meta.json without "skip_frame" hits cache when skip_frame="auto"
            meta_path.write_text(
                json.dumps(
                    {
                        "fps": 5.0,
                        "resolved_region": {"x": 0, "y": 756, "width": 1920, "height": 324},
                        "auto_region_detected": True,
                    }
                ),
                encoding="utf-8",
            )

            with (
                mock.patch("tools.vlm_ocr.timing_adapter.probe_video_stream_info", return_value=(1920, 1080, 10000)),
                mock.patch("tools.vlm_ocr.timing_adapter.subprocess.run") as mock_run,
            ):
                slots, _reg, meta = run_timing_stage(
                    video_path,
                    work_dir,
                    cli_bin=cli_bin,
                    fps=5.0,
                    skip_frame="auto",
                    reuse_cache=True,
                    quiet=True,
                )
                self.assertEqual(len(slots), 1)
                self.assertEqual(mock_run.call_count, 0)

                header, slot_iter, _meta2 = stream_timing_stage(
                    video_path,
                    work_dir,
                    cli_bin=cli_bin,
                    fps=5.0,
                    skip_frame="auto",
                    reuse_cache=True,
                    quiet=True,
                )
                self.assertEqual(header.source, "sublift_cli_cached")
                self.assertEqual(len(list(slot_iter)), 1)

                # Switching skip_frame to "noref" must invalidate cache and rerun CLI
                def _fake_run(cmd, capture_output=True, text=True, check=False):
                    del capture_output, text, check
                    out_idx = cmd.index("-o") + 1
                    Path(cmd[out_idx]).write_text('{"index":1,"start_ms":1200,"end_ms":2200}\n', encoding="utf-8")
                    res = mock.MagicMock()
                    res.returncode = 0
                    res.stdout = "开始处理视频: fake.mp4\n"
                    res.stderr = ""
                    return res

                mock_run.side_effect = _fake_run
                slots_nr, _reg2, meta_nr = run_timing_stage(
                    video_path,
                    work_dir,
                    cli_bin=cli_bin,
                    fps=5.0,
                    skip_frame="noref",
                    reuse_cache=True,
                    quiet=True,
                )
                self.assertEqual(mock_run.call_count, 1)
                self.assertEqual(meta_nr["skip_frame"], "noref")
                self.assertEqual(slots_nr[0].start_ms, 1200)

            # stream_timing_stage must also invalidate cache when requesting "none" against cached "noref"
            fake_popen = mock.MagicMock()
            fake_popen.stdout = io.StringIO(
                "开始处理视频: fake.mp4\n"
                "[字幕 #1] 00:00:01,000 --> 00:00:02,000 |  (置信度: 0.00)\n"
            )
            fake_popen.wait.return_value = 0
            fake_popen.poll.return_value = 0
            with (
                mock.patch("tools.vlm_ocr.timing_adapter.probe_video_stream_info", return_value=(1920, 1080, 10000)),
                mock.patch(
                    "tools.vlm_ocr.timing_adapter.probe_video_rich_info",
                    return_value=VideoStreamMeta(1920, 1080, 10000, 60.0, "hevc", "yuv420p"),
                ),
                mock.patch("tools.vlm_ocr.timing_adapter.subprocess.Popen", return_value=fake_popen) as mock_popen,
            ):
                header2, iter2, meta3 = stream_timing_stage(
                    video_path,
                    work_dir,
                    cli_bin=cli_bin,
                    fps=5.0,
                    skip_frame="none",
                    reuse_cache=True,
                    quiet=True,
                )
                out_slots = list(iter2)
                self.assertEqual(header2.source, "sublift_cli_stream")
                self.assertEqual(mock_popen.call_count, 1)
                called_cmd = mock_popen.call_args[0][0]
                self.assertEqual(called_cmd[called_cmd.index("--skip-frame") + 1], "none")
                self.assertEqual(meta3["skip_frame"], "none")
                self.assertEqual(len(out_slots), 1)

    def test_progressive_segment_merger_parity_and_early_streaming(self) -> None:
        rng = random.Random(42)
        for seed_idx in range(220):
            dur_ms = rng.randint(25_000, 120_000)
            seg_s = rng.choice([8.0, 12.0, 20.0])
            overlap_ms = 500
            windows = compute_segment_windows(dur_ms, seg_s, overlap_ms=overlap_ms)

            # Generate realistic cues including 0ms adjacent dialogue gaps and boundary straddlers
            global_cues: list[tuple[int, int]] = []
            cur_t = rng.randint(0, 1500)
            while cur_t + 800 < dur_ms:
                dur = rng.randint(400, 3200)
                end_t = min(dur_ms, cur_t + dur)
                global_cues.append((cur_t, end_t))
                if rng.random() < 0.25:
                    cur_t = end_t  # 0ms dialogue gap
                else:
                    cur_t = end_t + rng.randint(40, 2500)

            seg_events: list[list[tuple[int, int]]] = [[] for _ in windows]
            for seg_idx, (_cs, _ce, ws, we) in enumerate(windows):
                for s_ms, e_ms in global_cues:
                    if e_ms > ws and s_ms < we:
                        seg_events[seg_idx].append((max(ws, s_ms), min(we, e_ms)))
                # Occasionally inject a tiny boundary-crossing stutter event
                if seg_idx + 1 < len(windows) and rng.random() < 0.35:
                    b = windows[seg_idx][1]
                    if b - 350 >= ws and b + 350 <= windows[seg_idx + 1][3]:
                        seg_events[seg_idx].append((b - 350, b + 50))
                        seg_events[seg_idx + 1].append((b - 20, b + 350))
                seg_events[seg_idx].sort()

            batch_input = [
                (windows[i][0], windows[i][1], seg_events[i]) for i in range(len(windows))
            ]
            expected = merge_segment_timing_events(batch_input, overlap_ms=overlap_ms)

            merger = ProgressiveSegmentMerger(windows, overlap_ms=overlap_ms)
            live_emitted: list[TimingSlot] = []
            cursors = [0] * len(windows)
            finished = [False] * len(windows)
            while not all(finished):
                pending = [i for i, done in enumerate(finished) if not done]
                # Even on reverse-segment completion order in some seeds, parity must hold
                idx = rng.choice(pending) if seed_idx % 5 != 0 else pending[-1]
                if cursors[idx] < len(seg_events[idx]):
                    s_ms, e_ms = seg_events[idx][cursors[idx]]
                    cursors[idx] += 1
                    live_emitted.extend(merger.push_segment_event(idx, s_ms, e_ms))
                else:
                    finished[idx] = True
                    live_emitted.extend(merger.finish_segment(idx))

            self.assertEqual(
                [(s.slot_id, s.start_ms, s.end_ms) for s in live_emitted],
                [(s.slot_id, s.start_ms, s.end_ms) for s in expected],
                msg=f"Parity mismatch at seed_idx={seed_idx}",
            )

        # Verify Segment 0 yields safe slots live BEFORE Segment 1 unblocks
        class _QueueReader:
            def __init__(self, q: queue.Queue[str]) -> None:
                self._q = q

            def readline(self) -> str:
                return self._q.get()

            def close(self) -> None:
                return None

        seg1_release = threading.Event()
        seg1_done_flag = threading.Event()

        def _fake_popen(cmd, **_kwargs):  # type: ignore[no-untyped-def]
            out_jsonl = Path(cmd[cmd.index("-o") + 1])
            start_ms = int(cmd[cmd.index("--start-ms") + 1]) if "--start-ms" in cmd else 0
            q: queue.Queue[str] = queue.Queue()
            if start_ms == 0:
                q.put("[字幕 #1] 00:00:01,000 --> 00:00:03,000 | TIMING_ONLY (置信度: 0.00)\n")
                q.put("[字幕 #2] 00:00:10,000 --> 00:00:12,000 | TIMING_ONLY (置信度: 0.00)\n")
                out_jsonl.write_text(
                    '{"index":1,"start_ms":1000,"end_ms":3000}\n'
                    '{"index":2,"start_ms":10000,"end_ms":12000}\n',
                    encoding="utf-8",
                )
                q.put("")

                class _Seg0Proc:
                    stdout = _QueueReader(q)

                    def poll(self):
                        return 0

                    def wait(self, timeout=None):  # noqa: ARG002
                        return 0

                    def terminate(self):
                        return None

                    def kill(self):
                        return None

                return _Seg0Proc()

            def _emit_seg1() -> None:
                seg1_release.wait(timeout=5.0)
                q.put("[字幕 #1] 00:01:05,000 --> 00:01:07,000 | TIMING_ONLY (置信度: 0.00)\n")
                out_jsonl.write_text(
                    '{"index":1,"start_ms":65000,"end_ms":67000}\n',
                    encoding="utf-8",
                )
                q.put("")
                seg1_done_flag.set()

            threading.Thread(target=_emit_seg1, daemon=True).start()

            class _Seg1Proc:
                stdout = _QueueReader(q)

                def poll(self):
                    return 0 if seg1_done_flag.is_set() else None

                def wait(self, timeout=None):  # noqa: ARG002
                    seg1_release.wait(timeout=5.0)
                    return 0

                def terminate(self):
                    seg1_release.set()

                def kill(self):
                    seg1_release.set()

            return _Seg1Proc()

        with tempfile.TemporaryDirectory() as tmp:
            work_dir = Path(tmp)
            vid_meta = VideoStreamMeta(
                width=1920,
                height=1080,
                duration_ms=120_000,
                fps=29.97,
                codec_name="h264",
                pix_fmt="yuv420p",
                bit_rate_bps=5_000_000,
            )
            with mock.patch("tools.vlm_ocr.timing_adapter.subprocess.Popen", side_effect=_fake_popen):
                try:
                    header, slot_iter, meta = _run_segmented_timing_stream(
                        work_dir / "v.mp4",
                        work_dir,
                        cli_bin=Path("/usr/bin/true"),
                        vid_meta=vid_meta,
                        fps=5.0,
                        region=CropBox(0, 800, 1920, 120),
                        auto_region=False,
                        bottom_ratio=0.3,
                        threads=2,
                        hwaccel="auto",
                        skip_frame="auto",
                        max_analysis_width=1920,
                        workers=2,
                        segment_seconds=60.0,
                        quiet=True,
                    )
                    self.assertEqual(header.source, "sublift_cli_segmented")
                    first_slot = next(slot_iter)
                    self.assertEqual((first_slot.slot_id, first_slot.start_ms, first_slot.end_ms), (1, 1000, 3000))
                    # Must have arrived BEFORE Segment 1 unblocked!
                    self.assertFalse(seg1_done_flag.is_set())
                    seg1_release.set()
                    all_slots = [first_slot, *list(slot_iter)]
                    self.assertEqual(
                        [(s.slot_id, s.start_ms, s.end_ms) for s in all_slots],
                        [(1, 1000, 3000), (2, 10000, 12000), (3, 65000, 67000)],
                    )
                    self.assertEqual(meta["slot_count"], 3)
                finally:
                    seg1_release.set()

    def test_long_video_bitrate_and_multi_vt_parallelism_routing(self) -> None:
        juq_meta = VideoStreamMeta(
            width=1920,
            height=1080,
            duration_ms=7_200_000,
            fps=29.97,
            codec_name="h264",
            pix_fmt="yuv420p",
            bit_rate_bps=5_720_000,
        )
        hevc_meta = VideoStreamMeta(
            width=1920,
            height=1080,
            duration_ms=7_200_000,
            fps=29.97,
            codec_name="hevc",
            pix_fmt="yuv420p",
            bit_rate_bps=5_000_000,
        )
        s01e13_meta = VideoStreamMeta(
            width=1920,
            height=1080,
            duration_ms=3_785_000,
            fps=29.97,
            codec_name="h264",
            pix_fmt="yuv420p",
            bit_rate_bps=1_260_000,
        )
        synthetic_hd = VideoStreamMeta(
            width=1920,
            height=1080,
            duration_ms=3_800_000,
            fps=24.0,
            codec_name="h264",
            pix_fmt="yuv420p",
            bit_rate_bps=0,
        )
        uhd_meta = VideoStreamMeta(
            width=3840,
            height=2160,
            duration_ms=2_400_000,
            fps=24.0,
            codec_name="hevc",
            pix_fmt="yuv420p10le",
            bit_rate_bps=15_000_000,
        )
        with mock.patch("tools.vlm_ocr.timing_adapter.sys.platform", "darwin"):
            self.assertEqual(resolve_timing_parallelism(juq_meta), (3, 600.0))
            self.assertEqual(resolve_segment_hwaccel(juq_meta, "auto", 3), "videotoolbox")

            self.assertEqual(resolve_timing_parallelism(hevc_meta), (2, 600.0))
            self.assertEqual(resolve_segment_hwaccel(hevc_meta, "auto", 2), "videotoolbox")

            self.assertEqual(resolve_timing_parallelism(s01e13_meta), (2, 600.0))
            self.assertEqual(resolve_segment_hwaccel(s01e13_meta, "auto", 2), "none")

            self.assertEqual(resolve_timing_parallelism(synthetic_hd), (1, 0.0))
            self.assertEqual(resolve_timing_parallelism(uhd_meta), (1, 0.0))

    def test_single_pass_ppm_strip_consumption_and_immediate_unlink(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            strips_dir = root / ".strips_stream_tmp"
            strips_dir.mkdir(parents=True, exist_ok=True)
            w, h = 200, 40
            ppm_path = strips_dir / "strip_1000_2600.ppm"
            pixels = bytearray()
            for _y in range(h):
                for x in range(w):
                    if x < 20 or x >= 180:
                        pixels.extend((255, 0, 0))
                    else:
                        pixels.extend((0, 200, 0))
            ppm_path.write_bytes(f"P6\n{w} {h}\n255\n".encode("ascii") + bytes(pixels))

            region = CropBox(0, 760, 1920, 156)
            extractor = InMemoryStripExtractor(
                root / "missing.mp4",
                region,
                strip_w=160,
                strip_h=40,
                hwaccel="auto",
                single_pass_strips_dir=strips_dir,
                resolved_region=region,
                video_w=1920,
                quiet=True,
            )
            slot = TimingSlot(1, 1, 1000, 2600, 1800, 1600)
            img = extractor.extract_strip_in_memory(slot)
            self.assertFalse(ppm_path.exists())
            self.assertEqual(img.size, (160, 40))
            left_px = img.getpixel((2, 20))
            right_px = img.getpixel((157, 20))
            self.assertLess(left_px[0], 30)
            self.assertGreater(left_px[1], 150)
            self.assertLess(right_px[0], 30)
            self.assertGreater(right_px[1], 150)
            self.assertEqual(
                extractor.stats(),
                {
                    "cv2_seeks": 0,
                    "cv2_grabs": 0,
                    "ffmpeg_fallbacks": 0,
                    "single_pass_strip_hits": 1,
                },
            )
            extractor.close()
            self.assertFalse(strips_dir.exists())

    def test_tail_latency_speculative_hedging_wins_without_zombie_block(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            work_dir = Path(tmp)
            slots = [TimingSlot(1, 1, 1000, 2500, 1750, 1500)]
            primary_started = threading.Event()
            primary_release = threading.Event()
            call_lock = threading.Lock()
            call_count = 0

            def fake_prepare(sheet_idx: int, batch_slots: list[TimingSlot]) -> PreparedSheet:
                strips = {
                    s.slot_id: Image.new("RGB", (320, 40), (15, 25, 35))
                    for s in batch_slots
                }
                canvas = compose_contact_sheet_image(
                    batch_slots, strips, sheet_index=sheet_idx, strip_w=320, strip_h=40
                )
                return PreparedSheet(
                    spec=SheetSpec(
                        sheet_index=sheet_idx,
                        sheet_name=f"sheet_{sheet_idx:02d}.png",
                        sheet_path=f"memory://sheet_{sheet_idx:02d}.png",
                        expected_slot_ids=[s.slot_id for s in batch_slots],
                    ),
                    png_bytes=encode_image_png_bytes(canvas, compress_level=1),
                    batch_slots=list(batch_slots),
                    strip_images=strips,
                    decode_s=0.001,
                    encode_s=0.001,
                )

            def fake_urlopen(req: object, timeout: float = 120.0) -> _FakeHttpResponse:  # noqa: ARG001
                nonlocal call_count
                with call_lock:
                    call_count += 1
                    my_call = call_count
                if my_call == 1:
                    primary_started.set()
                    primary_release.wait(timeout=5.0)
                    txt = "主请求慢结果"
                else:
                    primary_started.wait(timeout=2.0)
                    txt = "对冲快赢结果"
                return _FakeHttpResponse(
                    {
                        "choices": [
                            {
                                "message": {
                                    "content": json.dumps(
                                        {"slots": [{"id": 1, "category": "dialogue", "lines": [txt]}]}
                                    )
                                }
                            }
                        ],
                        "usage": {"prompt_tokens": 30, "completion_tokens": 10, "total_tokens": 40},
                    }
                )

            cfg = VlmEndpointConfig(
                endpoint="https://api.minimax.cn/v1/chat/completions",
                api_key="test-key",
                model="MiniMax-M3.1-Flash",
                thinking_mode="auto",
            )
            try:
                with mock.patch("urllib.request.urlopen", side_effect=fake_urlopen):
                    _doc, transcribed, _all_slots, stats = transcribe_streaming_sheets(
                        iter(slots),
                        fake_prepare,
                        work_dir,
                        config=cfg,
                        slots_per_sheet=4,
                        strip_w=320,
                        strip_h=40,
                        extract_workers=2,
                        vlm_workers=2,
                        reuse_cache=False,
                        tail_hedge_min_s=0.0,
                        tail_hedge_factor=0.0,
                        quiet=True,
                    )
                self.assertFalse(primary_release.is_set())
                self.assertEqual(stats["hedged_requests"], 1)
                self.assertEqual(stats["hedged_wins"], 1)
                self.assertEqual(transcribed[0].lines, ["对冲快赢结果"])
            finally:
                primary_release.set()

    def test_slow_vlm_backpressure_does_not_block_stage1_slot_stream(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            work_dir = Path(tmp)
            slots = [
                TimingSlot(i, i, i * 1000, i * 1000 + 800, i * 1000 + 400, 800)
                for i in range(1, 7)
            ]
            stage1_finished_evt = threading.Event()
            observed_stage1_done_during_first_vlm_call = False
            call_lock = threading.Lock()
            call_count = 0

            def _gen_slots():
                yield from slots
                stage1_finished_evt.set()

            def fake_prepare(sheet_idx: int, batch_slots: list[TimingSlot]) -> PreparedSheet:
                strips = {
                    s.slot_id: Image.new("RGB", (320, 40), (20, 30, 40))
                    for s in batch_slots
                }
                canvas = compose_contact_sheet_image(
                    batch_slots, strips, sheet_index=sheet_idx, strip_w=320, strip_h=40
                )
                return PreparedSheet(
                    spec=SheetSpec(
                        sheet_index=sheet_idx,
                        sheet_name=f"sheet_{sheet_idx:02d}.png",
                        sheet_path=f"memory://sheet_{sheet_idx:02d}.png",
                        expected_slot_ids=[s.slot_id for s in batch_slots],
                    ),
                    png_bytes=encode_image_png_bytes(canvas, compress_level=1),
                    batch_slots=list(batch_slots),
                    strip_images=strips,
                    decode_s=0.001,
                    encode_s=0.001,
                )

            def fake_urlopen(req: object, timeout: float = 120.0) -> _FakeHttpResponse:  # noqa: ARG001
                nonlocal call_count, observed_stage1_done_during_first_vlm_call
                with call_lock:
                    call_count += 1
                    my_call = call_count
                if my_call == 1:
                    observed_stage1_done_during_first_vlm_call = stage1_finished_evt.wait(timeout=3.0)
                body = json.loads(req.data.decode("utf-8"))  # type: ignore[attr-defined]
                prompt_text = body["messages"][1]["content"][1]["text"]
                m = re.search(r"sheet_(\d+)\.png", prompt_text)
                idx = int(m.group(1)) if m else my_call
                start_id = (idx - 1) * 2 + 1
                resp_slots = [
                    {"id": start_id, "category": "dialogue", "lines": [f"句{start_id}"]},
                    {"id": start_id + 1, "category": "dialogue", "lines": [f"句{start_id + 1}"]},
                ]
                return _FakeHttpResponse(
                    {
                        "choices": [{"message": {"content": json.dumps({"slots": resp_slots})}}],
                        "usage": {"prompt_tokens": 30, "completion_tokens": 10, "total_tokens": 40},
                    }
                )

            cfg = VlmEndpointConfig(
                endpoint="https://api.minimax.cn/v1/chat/completions",
                api_key="test-key",
                model="MiniMax-M3.1-Flash",
                thinking_mode="auto",
            )
            with mock.patch("urllib.request.urlopen", side_effect=fake_urlopen):
                _doc, transcribed, all_slots, _stats = transcribe_streaming_sheets(
                    _gen_slots(),
                    fake_prepare,
                    work_dir,
                    config=cfg,
                    slots_per_sheet=2,
                    strip_w=320,
                    strip_h=40,
                    extract_workers=1,
                    vlm_workers=1,
                    max_in_flight_sheets=1,
                    reuse_cache=False,
                    quiet=True,
                )
            self.assertTrue(observed_stage1_done_during_first_vlm_call)
            self.assertTrue(stage1_finished_evt.is_set())
            self.assertEqual(len(all_slots), 6)
            self.assertEqual([s.lines[0] for s in transcribed], [f"句{i}" for i in range(1, 7)])


if __name__ == "__main__":
    unittest.main()
