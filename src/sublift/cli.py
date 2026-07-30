"""SubLift CLI 入口。"""

from __future__ import annotations

import argparse
import contextlib
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any, cast

from sublift.config import DEFAULT_CONFIG, Config
from sublift.detector import BottomCropDetector
from sublift.export import SrtExporter
from sublift.extractor import FfmpegExtractor
from sublift.models import SCRIPT_AUTO, SCRIPT_VALUES, SubtitleEntry
from sublift.ocr import (
    MockOcrEngine,
    PaddleOcrEngine,
    VisionOcrEngine,
    is_paddle_available,
    is_vision_available,
)
from sublift.pipeline import Pipeline
from sublift.runtime import (
    ResolutionSource,
    WorkerChoice,
    probe_cpp_paddle_available,
    resolve_runtime,
)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="sublift",
        description="硬字幕提取工具：从视频画面中识别烧录字幕并导出为字幕文件。",
    )
    subparsers = parser.add_subparsers(dest="command")

    extract_parser = subparsers.add_parser("extract", help="从视频提取硬字幕并导出为字幕文件")
    extract_parser.add_argument("video", help="输入视频文件路径")
    extract_parser.add_argument(
        "-o", "--output", default="output.srt", help="输出字幕文件路径（默认 output.srt）"
    )
    extract_parser.add_argument(
        "--fps",
        type=float,
        default=5.0,
        help="帧采样率（默认 5.0 fps）",
    )
    extract_parser.add_argument(
        "--confidence",
        type=float,
        default=0.5,
        help="OCR 置信度阈值（默认 0.5）",
    )
    extract_parser.add_argument(
        "--engine",
        choices=["vision", "mock", "paddle"],
        default="vision",
        help="OCR 引擎（默认 vision；mock 用于流程验证；paddle 跨平台）",
    )
    extract_parser.add_argument(
        "--runtime",
        choices=["python", "cpp"],
        default=None,
        help="执行运行时（默认 cpp；Paddle C++ 不可用时显式回退 Python）",
    )
    extract_parser.add_argument(
        "--script",
        choices=sorted(SCRIPT_VALUES),
        default=SCRIPT_AUTO,
        help="字幕文字系统（默认 auto；可选 cjk/latin）",
    )
    return parser


def main(argv: list[str] | None = None) -> None:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.command is None:
        parser.print_help()
        return
    if args.command == "extract":
        _run_extract(
            video=Path(args.video),
            output=Path(args.output),
            fps=args.fps,
            confidence=args.confidence,
            engine=args.engine,
            runtime=args.runtime,
            script=args.script,
        )


def _run_extract(
    *,
    video: Path,
    output: Path,
    fps: float,
    confidence: float,
    engine: str,
    runtime: str | None,
    script: str,
) -> None:
    """执行端到端字幕提取。"""
    if not video.exists():
        print(f"错误：视频文件不存在: {video}", file=sys.stderr)
        sys.exit(1)

    cpp_paddle = probe_cpp_paddle_available()
    choice = resolve_runtime(
        requested_runtime=runtime,
        requested_engine=engine,
        cpp_paddle_available=cpp_paddle,
    )

    if choice.resolved_via == ResolutionSource.PADDLE_OVERRIDE:
        print(
            "[SubLift] engine=paddle runtime=python model=PP-OCRv6-small "
            "status=fallback via=paddle_override；C++ Paddle 不可用（未构建/无模型），"
            "可设置 SUBLIFT_PADDLE_MODEL_DIR 或启用 SUBLIFT_ENABLE_PADDLE 构建。",
            file=sys.stderr,
        )
    elif choice.engine == "paddle":
        print(
            f"[SubLift] engine=paddle runtime={choice.runtime} "
            "model=PP-OCRv6-small status=stable "
            f"via={choice.resolved_via.value}",
            file=sys.stderr,
        )

    if choice.runtime == "cpp":
        _run_extract_cpp(
            video=video,
            output=output,
            fps=fps,
            confidence=confidence,
            script=script,
            choice=choice,
        )
    else:
        _run_extract_python(
            video=video,
            output=output,
            fps=fps,
            confidence=confidence,
            engine=choice.engine,
            script=script,
        )


def _find_sublift_worker_bin() -> Path | None:
    from sublift.worker_bin import resolve_worker_bin

    return resolve_worker_bin(Path(__file__).resolve().parents[2])


def _write_framed_json(sock: socket.socket, data: dict[str, Any]) -> None:
    raw_json = json.dumps(data).encode("utf-8")
    prefix = len(raw_json).to_bytes(4, byteorder="big")
    sock.sendall(prefix + raw_json)


def _read_framed_json(sock: socket.socket) -> dict[str, Any]:
    prefix = bytearray()
    while len(prefix) < 4:
        chunk = sock.recv(4 - len(prefix))
        if not chunk:
            raise RuntimeError("IPC Socket 读取长度前缀失败 (EOF)")
        prefix.extend(chunk)
    length = int.from_bytes(prefix, byteorder="big")
    body = bytearray()
    while len(body) < length:
        chunk = sock.recv(min(length - len(body), 65536))
        if not chunk:
            raise RuntimeError("IPC Socket 读取消息体不完整 (EOF)")
        body.extend(chunk)
    res = json.loads(body.decode("utf-8"))
    return cast(dict[str, Any], res)


def _run_extract_cpp(
    *,
    video: Path,
    output: Path,
    fps: float,
    confidence: float,
    script: str,
    choice: WorkerChoice,
) -> None:
    worker_bin = _find_sublift_worker_bin()
    if worker_bin is None:
        print(
            "错误：未找到 C++ worker 可执行文件 (sublift_worker)。\n"
            "请先使用 cmake 构建 C++ core 或使用 --runtime python 显式降级。",
            file=sys.stderr,
        )
        sys.exit(1)

    with tempfile.TemporaryDirectory() as tmp_dir_str:
        sock_path = Path(tmp_dir_str) / "worker.sock"

        proc = subprocess.Popen(
            [str(worker_bin), "--socket", str(sock_path), "--engine", choice.engine],
            stdout=subprocess.DEVNULL,
            stderr=(
                None
                if os.environ.get("SUBLIFT_PADDLE_PERF_DIAGNOSTICS") == "1"
                else subprocess.DEVNULL
            ),
        )

        client: socket.socket | None = None
        try:
            for _ in range(30):
                if sock_path.exists():
                    break
                if proc.poll() is not None:
                    print(
                        f"错误：C++ worker 异常退出 (exit code: {proc.returncode})",
                        file=sys.stderr,
                    )
                    sys.exit(1)
                time.sleep(0.1)

            if not sock_path.exists():
                print("错误：C++ worker 启动超时 (UDS socket 未出现)", file=sys.stderr)
                sys.exit(1)

            client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            client.connect(str(sock_path))

            _write_framed_json(
                client,
                {"type": "hello", "client": "sublift_cli", "protocol_version": 1},
            )
            bye = _read_framed_json(client)
            if bye.get("type") != "bye":
                print("错误：C++ worker 握手失败", file=sys.stderr)
                sys.exit(1)

            print(f"提取字幕 (C++ Worker)：{video}")
            print(
                f"采样率：{fps}fps  引擎：{choice.engine}  "
                f"置信度阈值：{confidence}  文字系统：{script}"
            )

            start = time.perf_counter()

            # subtitle_profile script-only hint: geometry zeros → worker applies
            # script to Config.subtitle_script and rebuilds band profile from crop
            # (matches Python Config.subtitle_script on in-process path).
            _write_framed_json(
                client,
                {
                    "type": "start_job",
                    "video_id": "cli_job",
                    "fps": fps,
                    "engine": choice.engine,
                    "confidence_threshold": confidence,
                    "video_path": str(video.resolve()),
                    "subtitle_profile": {
                        "script": script,
                        "center_x": 0,
                        "center_y": 0,
                        "height": 0,
                        "y_min": 0,
                        "y_max": 0,
                    },
                },
            )

            entries_data: list[dict[str, Any]] = []

            while True:
                msg = _read_framed_json(client)
                msg_type = msg.get("type")
                if msg_type == "progress":
                    pct = float(msg.get("pct", 0.0))
                    stage = str(msg.get("stage", ""))
                    if sys.stdout.isatty():
                        sys.stdout.write(f"\r[2/3] C++ 提帧进度: {pct * 100:.1f}% ({stage})")
                        sys.stdout.flush()
                elif msg_type == "entries":
                    entries_data = list(msg.get("entries", []))
                elif msg_type == "done":
                    if not msg.get("ok", False):
                        err = msg.get("error", "未知错误")
                        print(f"\n错误：C++ worker 执行失败 ({err})", file=sys.stderr)
                        sys.exit(1)
                    break
                elif msg_type == "error":
                    err = str(msg.get("message", "未知 IPC 错误"))
                    print(f"\n错误：C++ worker 发生协议错误 ({err})", file=sys.stderr)
                    sys.exit(1)

            if sys.stdout.isatty():
                sys.stdout.write("\n")

            sys.stdout.write("[3/3] 正在整理并导出字幕...\n")
            sys.stdout.flush()

            entries = [
                SubtitleEntry(
                    start_ms=int(e["start_ms"]),
                    end_ms=int(e["end_ms"]),
                    text=str(e["text"]),
                    confidence=float(e.get("confidence", 1.0)),
                )
                for e in entries_data
            ]
            elapsed = time.perf_counter() - start

            SrtExporter().export(entries, output)
            written = sum(1 for e in entries if e.text and e.text.strip())
            print(f"完成：{written} 条字幕 → {output}（耗时 {elapsed:.1f}s）")

        finally:
            if client is not None:
                with contextlib.suppress(Exception):
                    client.close()
            if proc is not None:
                try:
                    proc.terminate()
                    proc.wait(timeout=1.0)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
                except Exception:
                    pass


def _run_extract_python(
    *,
    video: Path,
    output: Path,
    fps: float,
    confidence: float,
    engine: str,
    script: str,
) -> None:
    print("[1/3] 正在探测视频...")
    from sublift.extractor.ffmpeg_extractor import probe_video

    try:
        info = probe_video(video)
        width, height = info.width, info.height
        duration_ms = info.duration_ms
        print(f"  分辨率: {width}x{height}  时长: {duration_ms / 1000:.1f}s")
    except Exception as e:
        print(f"  警告：无法探测视频属性 ({e})")
        duration_ms = 0

    ocr = _build_ocr_engine(engine)
    extractor = FfmpegExtractor(fps=fps)
    detector = BottomCropDetector(bottom_ratio=DEFAULT_CONFIG.region_bottom_ratio)
    config = Config(
        sample_fps=fps,
        confidence_threshold=confidence,
        subtitle_script=script,
    )

    pipeline = Pipeline(detector=detector, ocr=ocr, config=config, extractor=extractor)

    print(f"提取字幕：{video}")
    print(f"采样率：{fps}fps  引擎：{engine}  置信度阈值：{confidence}  文字系统：{script}")

    start = time.perf_counter()
    est_total_frames = int(duration_ms / 1000 * fps) if duration_ms > 0 else 0

    if sys.stdout.isatty():
        sys.stdout.write("[2/3] 提取与识别: 准备中...\r")
        sys.stdout.flush()
    else:
        print("[2/3] 提取与识别: 开始运行...")

    frames = extractor.extract(video)

    last_reported_pct = -1

    for frame in frames:
        event = pipeline.feed(frame)
        if event is not None:
            pipeline.ocr_segment(event)

        processed = pipeline.processed_count
        if est_total_frames > 0:
            pct = min(processed / est_total_frames, 1.0)
            if sys.stdout.isatty():
                bar_len = 30
                filled = int(pct * bar_len)
                if filled < bar_len:
                    bar = "=" * filled + ">" + " " * (bar_len - filled - 1)
                else:
                    bar = "=" * bar_len
                sys.stdout.write(
                    f"\r[2/3] 提取与识别: [{bar}] "
                    f"{pct * 100:.1f}% ({processed}/{est_total_frames} 帧)"
                )
                sys.stdout.flush()
            else:
                pct_10 = int(pct * 10)
                if pct_10 > last_reported_pct:
                    last_reported_pct = pct_10
                    print(f"[2/3] 提取与识别: {pct_10 * 10}% ({processed}/{est_total_frames} 帧)")
        else:
            if sys.stdout.isatty():
                sys.stdout.write(f"\r[2/3] 提取与识别: 已处理 {processed} 帧")
                sys.stdout.flush()
            else:
                if processed % 100 == 0:
                    print(f"[2/3] 提取与识别: 已处理 {processed} 帧")

    if sys.stdout.isatty():
        sys.stdout.write("\n")
        sys.stdout.flush()

    sys.stdout.write("[3/3] 正在整理并导出字幕...\n")
    sys.stdout.flush()

    entries = pipeline.finalize()
    elapsed = time.perf_counter() - start

    if os.environ.get("SUBLIFT_PADDLE_PERF_DIAGNOSTICS") == "1":
        print(
            "SUBLIFT_PADDLE_PERF_STATS "
            f"ocr_calls={pipeline.ocr_call_count}",
            file=sys.stderr,
        )

    SrtExporter().export(entries, output)
    written = sum(1 for e in entries if e.text and e.text.strip())
    print(f"完成：{written} 条字幕 → {output}（耗时 {elapsed:.1f}s）")


def _build_ocr_engine(
    engine: str,
) -> VisionOcrEngine | MockOcrEngine | PaddleOcrEngine:
    """构造 OCR 引擎。"""
    if engine == "vision":
        if not is_vision_available():
            print(
                "错误：Apple Vision 不可用。请安装 macOS 可选依赖：\n"
                "  uv sync --extra vision\n"
                "或使用 --engine mock 跑流程验证。",
                file=sys.stderr,
            )
            sys.exit(1)
        return VisionOcrEngine()
    if engine == "mock":
        return MockOcrEngine(text="[mock subtitle]", confidence=1.0)
    if engine == "paddle":
        if not is_paddle_available():
            print(
                "错误：PaddleOCR 不可用。请安装可选依赖：\n  uv sync --extra paddle",
                file=sys.stderr,
            )
            sys.exit(1)
        try:
            return PaddleOcrEngine()
        except Exception as exc:
            detail = str(exc) or type(exc).__name__
            print(
                "错误：PaddleOCR 初始化失败，无法加载或下载识别模型。\n"
                f"  原因：{detail}\n"
                "  请检查网络后重试；首次运行需要下载模型。\n"
                "  也可先预下载模型：\n"
                '  uv run --extra paddle python -c "from sublift.ocr import '
                'PaddleOcrEngine; PaddleOcrEngine()"'
                "",
                file=sys.stderr,
            )
            sys.exit(1)
    print(f"错误：未知引擎: {engine}", file=sys.stderr)
    sys.exit(1)


if __name__ == "__main__":
    main()
