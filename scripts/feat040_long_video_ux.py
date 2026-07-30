"""feat-040：真实长视频 path-mode 体验验收（与 GUI 默认 path mode 同源 IPC）。

用法::

    uv run python scripts/feat040_long_video_ux.py \\
      --video "/path/to/long.mp4" \\
      --region 0,880,1920,180 \\
      --out debug/feat040_ux

不把视频内容写入仓库；仅记录时长、分辨率、区域、时间戳日志与 RSS。
"""

from __future__ import annotations

import argparse
import asyncio
import json
import resource
import sys
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any

_REPO = Path(__file__).resolve().parents[1]
if str(_REPO) not in sys.path:
    sys.path.insert(0, str(_REPO))

from sublift.benchmark.srt import load_srt  # noqa: E402
from sublift.export.srt import SrtExporter  # noqa: E402
from sublift.extractor.ffmpeg_extractor import (  # noqa: E402
    probe_duration_ms,
    probe_source_frame,
)
from sublift.ipc.bridge import BridgeHandler  # noqa: E402
from sublift.ipc.protocol import build_cancel_job, build_start_job  # noqa: E402
from sublift.models import SubtitleEntry  # noqa: E402
from sublift.ocr.vision import VisionOcrEngine, is_vision_available  # noqa: E402


@dataclass
class Event:
    t_wall: float
    kind: str
    detail: dict[str, Any] = field(default_factory=dict)


def _rss_bytes() -> int:
    usage = resource.getrusage(resource.RUSAGE_SELF)
    # macOS: bytes
    return int(usage.ru_maxrss)


async def run_job(
    *,
    bridge: BridgeHandler,
    video: Path,
    region: list[int],
    duration_ms: int,
    fps: float,
    video_id: str,
    cancel_after_video_ms: int | None,
    max_wall_s: float | None,
) -> dict[str, Any]:
    """在给定 BridgeHandler 上跑一次 path mode；可选按视频进度取消。"""
    events: list[Event] = []
    t0 = time.perf_counter()
    push_entries: list[dict[str, Any]] = []
    final_entries: list[dict[str, Any]] = []
    progress_values: list[float] = []
    stages: list[str] = []
    first_entry_wall: float | None = None
    done_msg: dict[str, Any] | None = None
    rss_samples: list[dict[str, Any]] = [{"t": 0.0, "peak_rss": _rss_bytes()}]

    async def push(msg: dict[str, Any]) -> None:
        nonlocal first_entry_wall, done_msg, final_entries
        now = time.perf_counter() - t0
        mtype = msg.get("type")
        if mtype == "progress":
            # protocol 字段名为 pct（0.0~1.0），不是 percent
            raw_pct = msg.get("pct", msg.get("percent", 0.0))
            pct = float(raw_pct or 0.0)
            stage = str(msg.get("stage", ""))
            progress_values.append(pct)
            stages.append(stage)
            events.append(Event(now, "progress", {"stage": stage, "pct": pct}))
            if len(progress_values) % 20 == 0:
                rss_samples.append({"t": now, "peak_rss": _rss_bytes()})
        elif mtype == "push_entry":
            entry = msg.get("entry") or {}
            push_entries.append(dict(entry))
            if first_entry_wall is None:
                first_entry_wall = now
            events.append(
                Event(
                    now,
                    "push_entry",
                    {
                        "start_ms": entry.get("start_ms"),
                        "end_ms": entry.get("end_ms"),
                        "text": str(entry.get("text", ""))[:40],
                    },
                )
            )
        elif mtype == "entries":
            final_entries = [dict(e) for e in (msg.get("entries") or []) if isinstance(e, dict)]
            events.append(Event(now, "entries", {"n": len(final_entries)}))
        elif mtype == "done":
            done_msg = msg
            events.append(Event(now, "done", {"ok": msg.get("ok"), "error": msg.get("error")}))

    start = build_start_job(
        video_id,
        fps,
        "vision",
        0.5,
        duration_ms=duration_ms,
        region_box=region,
        video_path=str(video),
    )
    resp = await bridge.handle(start, push)
    events.append(
        Event(
            time.perf_counter() - t0,
            "start_response",
            {
                "type": resp.get("type") if resp else None,
                "stage": (resp or {}).get("stage"),
            },
        )
    )

    # 轮询 path_task；可选按视频时间取消
    cancel_requested_at: float | None = None
    ready_after_cancel_at: float | None = None
    while bridge._path_task is not None and not bridge._path_task.done():
        await asyncio.sleep(0.05)
        now = time.perf_counter() - t0
        if max_wall_s is not None and now > max_wall_s and cancel_requested_at is None:
            cancel_requested_at = now
            cancel_resp = await bridge.handle(build_cancel_job(video_id), push)
            ready_after_cancel_at = time.perf_counter() - t0
            events.append(
                Event(
                    ready_after_cancel_at,
                    "cancel_response",
                    {
                        "type": cancel_resp.get("type") if cancel_resp else None,
                        "error": (cancel_resp or {}).get("error"),
                        "cancel_latency_s": ready_after_cancel_at - cancel_requested_at,
                    },
                )
            )
            break
        if cancel_after_video_ms is not None and cancel_requested_at is None and progress_values:
            est_video_ms = progress_values[-1] * duration_ms
            if est_video_ms >= cancel_after_video_ms:
                cancel_requested_at = now
                cancel_resp = await bridge.handle(build_cancel_job(video_id), push)
                ready_after_cancel_at = time.perf_counter() - t0
                events.append(
                    Event(
                        ready_after_cancel_at,
                        "cancel_response",
                        {
                            "type": cancel_resp.get("type") if cancel_resp else None,
                            "error": (cancel_resp or {}).get("error"),
                            "cancel_latency_s": ready_after_cancel_at - cancel_requested_at,
                            "est_video_ms": est_video_ms,
                        },
                    )
                )
                break

    if bridge._path_task is not None:
        import contextlib

        with contextlib.suppress(TimeoutError, asyncio.CancelledError):
            await asyncio.wait_for(bridge._path_task, timeout=120.0)

    wall = time.perf_counter() - t0
    rss_samples.append({"t": wall, "peak_rss": _rss_bytes()})
    mono = all(
        progress_values[i + 1] + 1e-6 >= progress_values[i]
        for i in range(max(0, len(progress_values) - 1))
    )

    return {
        "video_id": video_id,
        "wall_s": wall,
        "first_entry_wall_s": first_entry_wall,
        "push_entry_count": len(push_entries),
        "final_entry_count": len(final_entries),
        "final_entries": final_entries,
        "entries_sample": (final_entries or push_entries)[:5],
        "progress_count": len(progress_values),
        "progress_monotonic": mono,
        "progress_min": min(progress_values) if progress_values else None,
        "progress_max": max(progress_values) if progress_values else None,
        "stages": stages[:20],
        "stage_set": sorted(set(stages)),
        "cancel_requested_at": cancel_requested_at,
        "ready_after_cancel_at": ready_after_cancel_at,
        "cancel_latency_s": (
            (ready_after_cancel_at - cancel_requested_at)
            if cancel_requested_at is not None and ready_after_cancel_at is not None
            else None
        ),
        "done": done_msg,
        "rss_samples": rss_samples,
        "peak_rss_bytes": max(s["peak_rss"] for s in rss_samples),
        "events": [asdict(e) for e in events],
    }


async def cancel_then_restart_same_bridge(
    *,
    video: Path,
    region: list[int],
    duration_ms: int,
    fps: float,
) -> dict[str, Any]:
    """同一 BridgeHandler：start → 进度≥30s 视频 → cancel → ≤5s 内 restart。"""
    bridge = BridgeHandler(ocr_engine_factory=VisionOcrEngine)
    cancel_phase = await run_job(
        bridge=bridge,
        video=video,
        region=region,
        duration_ms=duration_ms,
        fps=fps,
        video_id="feat040-cancel",
        cancel_after_video_ms=30_000,
        max_wall_s=120.0,
    )
    t_restart = time.perf_counter()
    restart_phase = await run_job(
        bridge=bridge,
        video=video,
        region=region,
        duration_ms=duration_ms,
        fps=fps,
        video_id="feat040-restart",
        cancel_after_video_ms=20_000,
        max_wall_s=60.0,
    )
    restart_gap = time.perf_counter() - t_restart
    restart_phase["restart_gap_s"] = restart_gap
    restart_phase["same_bridge"] = True
    return {"cancel": cancel_phase, "restart": restart_phase}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="feat-040 long video path-mode UX")
    parser.add_argument("--video", type=Path, required=True)
    parser.add_argument(
        "--region",
        type=str,
        default="0,880,1920,180",
        help="source-frame region x,y,w,h",
    )
    parser.add_argument("--fps", type=float, default=5.0)
    parser.add_argument("--out", type=Path, default=Path("debug/feat040_ux"))
    parser.add_argument(
        "--full-max-video-s",
        type=float,
        default=None,
        help="可选：完整跑时截断到该视频秒数（默认全片）",
    )
    args = parser.parse_args(argv)

    if not is_vision_available():
        print("Vision 不可用", file=sys.stderr)
        return 1
    if not args.video.is_file():
        print(f"视频不存在: {args.video}", file=sys.stderr)
        return 1

    region = [int(x) for x in args.region.split(",")]
    if len(region) != 4:
        print("region 必须是 x,y,w,h", file=sys.stderr)
        return 1

    info = probe_source_frame(args.video)
    duration_ms = probe_duration_ms(args.video)
    duration_s = duration_ms / 1000.0
    args.out.mkdir(parents=True, exist_ok=True)

    meta = {
        "video_path_basename": args.video.name,
        "video_path_sha_note": "path not stored beyond basename",
        "duration_s": duration_s,
        "duration_ms": duration_ms,
        "source_width": info.width,
        "source_height": info.height,
        "display_transform_ok": info.display_transform_ok,
        "region_box": region,
        "fps": args.fps,
        "engine": "vision",
        "mode": "path_mode_bridge",
        "note": "BridgeHandler path mode = GUI default product path",
        "date": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
    }
    (args.out / "meta.json").write_text(
        json.dumps(meta, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )

    report: dict[str, Any] = {"meta": meta, "phases": {}}

    # Phase A+B: 同一 BridgeHandler 上 cancel → restart（硬门）
    print("=== Phase A+B: same-bridge cancel≥30s video then restart ===", flush=True)
    cr = asyncio.run(
        cancel_then_restart_same_bridge(
            video=args.video,
            region=region,
            duration_ms=duration_ms,
            fps=args.fps,
        )
    )
    cancel_result = cr["cancel"]
    restart_result = cr["restart"]
    report["phases"]["cancel"] = cancel_result
    report["phases"]["restart"] = restart_result
    print(
        f"  cancel_latency_s={cancel_result.get('cancel_latency_s')} "
        f"restart_gap_s={restart_result.get('restart_gap_s')} "
        f"restart_progress_max={restart_result.get('progress_max')}",
        flush=True,
    )

    # Phase C: full run
    print("=== Phase C: full path-mode processing ===", flush=True)

    async def _full() -> dict[str, Any]:
        bridge = BridgeHandler(ocr_engine_factory=VisionOcrEngine)
        return await run_job(
            bridge=bridge,
            video=args.video,
            region=region,
            duration_ms=duration_ms,
            fps=args.fps,
            video_id="feat040-full",
            cancel_after_video_ms=None,
            max_wall_s=None,
        )

    full_result = asyncio.run(_full())
    # 不把完整字幕文本写入总报告（体积大）；单独 SRT
    final_entries = list(full_result.get("final_entries") or [])
    full_result_public = dict(full_result)
    full_result_public.pop("final_entries", None)
    report["phases"]["full"] = full_result_public
    print(
        f"  wall_s={full_result.get('wall_s'):.2f} "
        f"first_entry={full_result.get('first_entry_wall_s')} "
        f"final_entries={full_result.get('final_entry_count')} "
        f"peak_rss={full_result.get('peak_rss_bytes')}",
        flush=True,
    )

    # SRT 导出 + 再打开
    srt_path = args.out / "export.srt"
    subs = [
        SubtitleEntry(
            start_ms=int(e["start_ms"]),
            end_ms=int(e["end_ms"]),
            text=str(e.get("text", "")),
            confidence=float(e.get("confidence") or 0.0),
        )
        for e in final_entries
    ]
    srt_path.write_text(SrtExporter().format(subs), encoding="utf-8")
    reopened = load_srt(srt_path)
    srt_info = {
        "path": str(srt_path.name),
        "exported_count": len(subs),
        "reopened_count": len(reopened),
        "match": len(subs) == len(reopened) and len(subs) > 0,
        "first_text": reopened[0].text[:40] if reopened else None,
    }
    report["srt"] = srt_info
    print(
        f"  SRT export={srt_info['exported_count']} reopen={srt_info['reopened_count']} "
        f"match={srt_info['match']}",
        flush=True,
    )

    # Gates
    gates: dict[str, Any] = {}
    full = report["phases"]["full"]
    cancel = report["phases"]["cancel"]
    restart = report["phases"]["restart"]

    gates["duration_ge_10min"] = {
        "pass": duration_s >= 600,
        "duration_s": duration_s,
    }
    gates["first_entry_le_10s"] = {
        "pass": (
            full.get("first_entry_wall_s") is not None and float(full["first_entry_wall_s"]) <= 10.0
        ),
        "first_entry_wall_s": full.get("first_entry_wall_s"),
    }
    gates["progress_monotonic"] = {
        "pass": bool(full.get("progress_monotonic"))
        and float(full.get("progress_max") or 0) >= 0.99,
        "min": full.get("progress_min"),
        "max": full.get("progress_max"),
    }
    gates["completed_with_entries"] = {
        "pass": int(full.get("final_entry_count") or 0) >= 1,
        "final_entry_count": full.get("final_entry_count"),
        "push_entry_count": full.get("push_entry_count"),
    }
    gates["cancel_latency_le_1s"] = {
        "pass": (
            cancel.get("cancel_latency_s") is not None and float(cancel["cancel_latency_s"]) <= 1.0
        ),
        "cancel_latency_s": cancel.get("cancel_latency_s"),
        "metric": "bridge.handle(cancel) return latency (path-mode product path)",
    }
    restart_done = restart.get("done")
    restart_socket_error = (
        isinstance(restart_done, dict)
        and restart_done.get("ok") is False
        and "socket" in str(restart_done.get("error", "")).lower()
    )
    gates["same_bridge_restart_ok"] = {
        "pass": (
            bool(restart.get("same_bridge"))
            and restart.get("restart_gap_s") is not None
            and float(restart["restart_gap_s"]) <= 5.0
            and float(restart.get("progress_max") or 0) > 0
            and "processing" in set(restart.get("stage_set") or [])
            and not restart_socket_error
        ),
        "same_bridge": restart.get("same_bridge"),
        "restart_gap_s": restart.get("restart_gap_s"),
        "progress_max": restart.get("progress_max"),
        "stage_set": restart.get("stage_set"),
        "socket_error": restart_socket_error,
    }
    gates["srt_export_reopen"] = {
        "pass": bool(srt_info["match"]),
        **srt_info,
    }
    gates["rss_recorded"] = {
        "pass": bool(full.get("rss_samples"))
        and isinstance(full.get("peak_rss_bytes"), int)
        and int(full["peak_rss_bytes"]) > 0,
        "peak_rss_bytes": full.get("peak_rss_bytes"),
        "samples": len(full.get("rss_samples") or []),
    }

    report["gates"] = gates
    report["all_pass"] = all(bool(g.get("pass")) for g in gates.values())
    report["evidence_note"] = (
        "BridgeHandler path mode = GUI 默认产品通路（ADR-0010）；"
        "本脚本测提取/取消/重启/导出稳定性，不是 SwiftUI 点击/拖拽 UI。"
    )

    out_json = args.out / "ux_report.json"
    out_json.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {out_json}", flush=True)
    print("GATES", json.dumps(gates, ensure_ascii=False, indent=2), flush=True)
    print("ALL_PASS" if report["all_pass"] else "GATES_FAILED", flush=True)
    return 0 if report["all_pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
