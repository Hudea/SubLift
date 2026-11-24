"""Midpoint frame extraction and single-column Contact Sheet builder (streaming & disk modes)."""

from __future__ import annotations

import functools
import io
import json
import shutil
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Mapping

from PIL import Image, ImageDraw, ImageFont

from .models import CropBox, SheetSpec, TimingSlot


def compute_strip_crop_geometry(
    video_w: int,
    video_h: int,
    region: CropBox,
    *,
    horiz_margin_ratio: float = 0.10,
    vert_pad_px: int = 12,
) -> CropBox:
    """Compute padded strip crop box, trimming outer 10% margins for full-width bands."""
    if video_w <= 0 or video_h <= 0:
        raise ValueError(f"Invalid video dimensions: {video_w}x{video_h}")
    if region.x >= video_w or region.y >= video_h:
        raise ValueError(
            f"Region {region.to_cli_arg()} out of bounds for video dimensions {video_w}x{video_h}"
        )

    if region.x == 0 and region.width >= video_w:
        crop_x = max(0, round(video_w * horiz_margin_ratio))
        crop_w = max(1, min(video_w - crop_x, round(video_w * (1.0 - 2.0 * horiz_margin_ratio))))
    else:
        crop_x = max(0, region.x)
        crop_w = max(1, min(video_w - crop_x, region.width))

    crop_y = max(0, region.y - vert_pad_px)
    crop_y2 = min(video_h, region.y + region.height + vert_pad_px)
    min_h = min(16, video_h)
    crop_h = max(min_h, crop_y2 - crop_y)
    if crop_y + crop_h > video_h:
        crop_y = max(0, video_h - crop_h)
        crop_h = min(crop_h, video_h - crop_y)
    return CropBox(x=crop_x, y=crop_y, width=crop_w, height=crop_h)


def compute_target_strip_height(
    crop_box: CropBox,
    *,
    strip_w: int = 960,
    min_h: int = 90,
    max_h: int = 180,
) -> int:
    """Scale target strip height proportionally while clamping to [min_h, max_h]."""
    raw_h = round(strip_w * crop_box.height / max(1, crop_box.width))
    return max(min_h, min(max_h, raw_h))


@functools.lru_cache(maxsize=16)
def _load_font(size: int) -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
    for path in (
        "/System/Library/Fonts/Menlo.ttc",
        "/System/Library/Fonts/SFNSMono.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
    ):
        if Path(path).exists():
            try:
                return ImageFont.truetype(path, size)
            except Exception:
                continue
    return ImageFont.load_default()


def _log(msg: str, *, quiet: bool) -> None:
    if not quiet:
        print(msg, file=sys.stderr, flush=True)


def _resolve_single_frame_hw_modes(hwaccel: str) -> list[str]:
    norm_hw = (hwaccel or "").strip().lower()
    if norm_hw in ("", "none", "off", "false", "0", "auto"):
        return [""]
    return [hwaccel.strip(), ""]


def extract_midpoint_strips(
    video_path: Path,
    slots: list[TimingSlot],
    strip_dir: Path,
    *,
    crop_box: CropBox,
    strip_w: int = 960,
    strip_h: int | None = None,
    workers: int = 8,
    hwaccel: str = "videotoolbox",
    reuse_cache: bool = True,
    quiet: bool = False,
) -> dict[int, Path]:
    """Extract 1 midpoint PNG strip per TimingSlot via ffmpeg."""
    if not slots:
        strip_dir.mkdir(parents=True, exist_ok=True)
        return {}

    target_h = strip_h or compute_target_strip_height(crop_box, strip_w=strip_w)
    strip_dir.mkdir(parents=True, exist_ok=True)
    vf = (
        f"crop={crop_box.width}:{crop_box.height}:{crop_box.x}:{crop_box.y},"
        f"scale={strip_w}:{target_h}:flags=lanczos:force_original_aspect_ratio=decrease,"
        f"pad={strip_w}:{target_h}:(ow-iw)/2:(oh-ih)/2:color=black"
    )

    hw_modes = _resolve_single_frame_hw_modes(hwaccel)

    t0 = time.perf_counter()
    total = len(slots)
    _log(
        f"[Stage 2a] Extracting {total} midpoint strips ({strip_w}x{target_h}, workers={workers})...",
        quiet=quiet,
    )

    def _extract_one(slot: TimingSlot) -> tuple[int, Path]:
        out_path = strip_dir / f"slot_{slot.slot_id:03d}.png"
        if reuse_cache and out_path.is_file() and out_path.stat().st_size > 0:
            return slot.slot_id, out_path
        for seek_ms in (slot.mid_ms, slot.start_ms, max(0, slot.start_ms - 100)):
            seek_s = seek_ms / 1000.0
            for hw_mode in hw_modes:
                cmd = ["ffmpeg", "-y", "-v", "error"]
                if hw_mode:
                    cmd.extend(["-hwaccel", hw_mode])
                cmd.extend(
                    [
                        "-ss",
                        f"{seek_s:.3f}",
                        "-i",
                        str(video_path),
                        "-frames:v",
                        "1",
                        "-vf",
                        vf,
                        str(out_path),
                    ]
                )
                try:
                    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
                except subprocess.CalledProcessError:
                    if hw_mode:
                        continue
                    raise
                if out_path.is_file() and out_path.stat().st_size > 0:
                    return slot.slot_id, out_path
        raise RuntimeError(
            f"ffmpeg produced no frame for slot #{slot.slot_id} at {slot.mid_ms}ms"
        )

    result: dict[int, Path] = {}
    step_log = max(1, min(100, total // 10 or 1))
    with ThreadPoolExecutor(max_workers=max(1, workers)) as pool:
        futs = [pool.submit(_extract_one, slot) for slot in slots]
        for done_n, fut in enumerate(as_completed(futs), 1):
            sid, path = fut.result()
            result[sid] = path
            if done_n % step_log == 0 or done_n == total:
                _log(
                    f"[Stage 2a] {done_n}/{total} strips ready ({time.perf_counter() - t0:.1f}s)",
                    quiet=quiet,
                )
    return result


def compose_contact_sheet_image(
    batch_slots: Iterable[TimingSlot],
    strip_images_by_id: Mapping[int, Image.Image],
    *,
    sheet_index: int = 1,
    strip_w: int = 960,
    strip_h: int = 90,
    header_tag: str = "SubLift VLM OCR",
) -> Image.Image:
    """Compose a numbered single-column Contact Sheet PIL Image purely in memory."""
    batch_list = list(batch_slots)
    if not batch_list:
        raise ValueError("Cannot render an empty Contact Sheet")

    font_id = _load_font(22)
    font_ts = _load_font(13)
    font_hdr = _load_font(16)

    label_w = 180
    sep_w = 4
    row_gap = 6
    header_h = 44
    pad_x = 8
    canvas_w = pad_x + label_w + sep_w + strip_w + pad_x
    canvas_h = header_h + len(batch_list) * (strip_h + row_gap) + 8

    canvas = Image.new("RGB", (canvas_w, canvas_h), (18, 20, 24))
    draw = ImageDraw.Draw(canvas)
    draw.rectangle([0, 0, canvas_w, header_h], fill=(28, 32, 40))

    first_id = batch_list[0].slot_id
    last_id = batch_list[-1].slot_id
    draw.text(
        (14, 12),
        f"{header_tag} #{sheet_index:02d} | Slots #{first_id:03d}..#{last_id:03d}",
        fill=(235, 240, 248),
        font=font_hdr,
    )

    sep_x0 = pad_x + label_w
    strip_x0 = sep_x0 + sep_w

    for r, slot in enumerate(batch_list):
        y0 = header_h + r * (strip_h + row_gap)
        bg = (245, 247, 250) if r % 2 == 0 else (228, 234, 242)
        draw.rectangle([pad_x, y0, sep_x0 - 1, y0 + strip_h], fill=bg)
        draw.rectangle([sep_x0, y0, strip_x0 - 1, y0 + strip_h], fill=(56, 189, 248))
        draw.text((pad_x + 10, y0 + 16), f"#{slot.slot_id:03d}", fill=(15, 23, 42), font=font_id)
        draw.text(
            (pad_x + 10, y0 + 52),
            f"{slot.start_ms / 1000:.2f}s - {slot.end_ms / 1000:.2f}s",
            fill=(100, 116, 139),
            font=font_ts,
        )
        raw_strip = strip_images_by_id[slot.slot_id]
        strip_img = raw_strip if raw_strip.mode == "RGB" else raw_strip.convert("RGB")
        if strip_img.size != (strip_w, strip_h):
            strip_img = strip_img.resize((strip_w, strip_h), Image.Resampling.LANCZOS)
        canvas.paste(strip_img, (strip_x0, y0))

    return canvas


def encode_image_png_bytes(img: Image.Image, *, compress_level: int = 1) -> bytes:
    """Encode a PIL Image into PNG bytes in memory using fast zlib compression (GIL-releasing)."""
    buf = io.BytesIO()
    img.save(buf, format="PNG", compress_level=compress_level)
    return buf.getvalue()


@dataclass
class PreparedSheet:
    """In-memory encoded Contact Sheet paired with its decoded strip buffers for split-rescue."""

    spec: SheetSpec
    png_bytes: bytes
    batch_slots: list[TimingSlot]
    strip_images: dict[int, Image.Image]
    decode_s: float
    encode_s: float


class InMemoryStripExtractor:
    """Thread-safe persistent multi-core midpoint strip extractor operating in RAM."""

    def __init__(
        self,
        video_path: Path,
        crop: CropBox,
        *,
        strip_w: int = 960,
        strip_h: int = 90,
        hwaccel: str = "none",
        single_pass_strips_dir: Path | None = None,
        resolved_region: CropBox | None = None,
        video_w: int = 0,
        quiet: bool = False,
        save_debug_strips_dir: Path | None = None,
        save_debug_sheets_dir: Path | None = None,
    ) -> None:
        self.video_path = video_path
        self.crop = crop
        self.strip_w = strip_w
        self.strip_h = strip_h
        self.hwaccel = hwaccel
        self.single_pass_strips_dir = single_pass_strips_dir
        self.resolved_region = resolved_region
        self.video_w = video_w
        self.quiet = quiet
        self.save_debug_strips_dir = save_debug_strips_dir
        self.save_debug_sheets_dir = save_debug_sheets_dir

        self._tls = threading.local()
        self._lock = threading.Lock()
        self._open_caps: list[Any] = []
        self._closed = False
        self._warned_cv2_unavailable = False

        self.cv2_seeks = 0
        self.cv2_grabs = 0
        self.ffmpeg_fallbacks = 0
        self.single_pass_hits = 0

        try:
            import cv2  # type: ignore[import-not-found]
            try:
                cv2.setNumThreads(1)
            except Exception:
                pass
            self._cv2: Any = cv2
        except Exception:
            self._cv2 = None

        if self.save_debug_strips_dir is not None:
            self.save_debug_strips_dir.mkdir(parents=True, exist_ok=True)
        if self.save_debug_sheets_dir is not None:
            self.save_debug_sheets_dir.mkdir(parents=True, exist_ok=True)

    def __enter__(self) -> InMemoryStripExtractor:
        return self

    def __exit__(self, *_args: object) -> None:
        self.close()

    def close(self) -> None:
        with self._lock:
            self._closed = True
            caps = list(self._open_caps)
            self._open_caps.clear()
        for cap in caps:
            try:
                cap.release()
            except Exception:
                pass
        if self.single_pass_strips_dir is not None:
            shutil.rmtree(self.single_pass_strips_dir, ignore_errors=True)

    def stats(self) -> dict[str, int]:
        with self._lock:
            return {
                "cv2_seeks": self.cv2_seeks,
                "cv2_grabs": self.cv2_grabs,
                "ffmpeg_fallbacks": self.ffmpeg_fallbacks,
                "single_pass_strip_hits": self.single_pass_hits,
            }

    def _warn_cv2_once(self) -> None:
        should_warn = False
        with self._lock:
            if not self._warned_cv2_unavailable:
                self._warned_cv2_unavailable = True
                should_warn = True
        if should_warn:
            _log(
                "[Stage 2] Warning: OpenCV VideoCapture unavailable, falling back to ffmpeg subprocess per slot",
                quiet=self.quiet,
            )

    def _get_thread_capture(self) -> tuple[Any, float, float, float] | None:
        if self._closed:
            return None
        if self._cv2 is None:
            self._warn_cv2_once()
            return None
        state = getattr(self._tls, "cap_state", None)
        if state is None:
            cap = self._cv2.VideoCapture(str(self.video_path))
            if not cap.isOpened():
                cap.release()
                self._warn_cv2_once()
                return None
            fps = float(cap.get(self._cv2.CAP_PROP_FPS) or 0.0)
            if fps <= 0.0 or fps > 240.0:
                fps = 25.0
            frame_dur_ms = 1000.0 / fps
            fw = int(cap.get(self._cv2.CAP_PROP_FRAME_WIDTH) or 0)
            if fw <= 0:
                fw = self.crop.x + self.crop.width
            max_grab_gap_ms = 1200.0 if fw > 1920 else 5000.0
            state = {
                "cap": cap,
                "frame_dur_ms": frame_dur_ms,
                "cur_ms": -1e9,
                "max_grab_gap_ms": max_grab_gap_ms,
            }
            self._tls.cap_state = state
            with self._lock:
                if self._closed:
                    cap.release()
                    return None
                self._open_caps.append(cap)
        return (
            state["cap"],
            float(state["frame_dur_ms"]),
            float(state["cur_ms"]),
            float(state["max_grab_gap_ms"]),
        )

    def _set_thread_cur_ms(self, cur_ms: float) -> None:
        state = getattr(self._tls, "cap_state", None)
        if state is not None:
            state["cur_ms"] = cur_ms

    def _crop_and_resize_bgr(self, frame: Any) -> Image.Image:
        cv2 = self._cv2
        fh, fw = frame.shape[:2]
        x0 = max(0, min(self.crop.x, fw - 1))
        y0 = max(0, min(self.crop.y, fh - 1))
        x1 = max(x0 + 1, min(x0 + self.crop.width, fw))
        y1 = max(y0 + 1, min(y0 + self.crop.height, fh))
        roi = frame[y0:y1, x0:x1]
        resized = cv2.resize(roi, (self.strip_w, self.strip_h), interpolation=cv2.INTER_AREA)
        rgb = cv2.cvtColor(resized, cv2.COLOR_BGR2RGB)
        return Image.fromarray(rgb)

    def _extract_strip_ffmpeg_raw_pipe(self, slot: TimingSlot) -> Image.Image:
        with self._lock:
            self.ffmpeg_fallbacks += 1
        vf = (
            f"crop={self.crop.width}:{self.crop.height}:{self.crop.x}:{self.crop.y},"
            f"scale={self.strip_w}:{self.strip_h}:flags=lanczos:force_original_aspect_ratio=decrease,"
            f"pad={self.strip_w}:{self.strip_h}:(ow-iw)/2:(oh-ih)/2:color=black"
        )
        hw_modes = _resolve_single_frame_hw_modes(self.hwaccel)
        expected_bytes = self.strip_w * self.strip_h * 3

        for seek_ms in (slot.mid_ms, slot.start_ms, max(0, slot.start_ms - 100)):
            seek_s = seek_ms / 1000.0
            for hw_mode in hw_modes:
                cmd = ["ffmpeg", "-nostdin", "-v", "error"]
                if hw_mode:
                    cmd.extend(["-hwaccel", hw_mode])
                cmd.extend(
                    [
                        "-ss",
                        f"{seek_s:.3f}",
                        "-i",
                        str(self.video_path),
                        "-frames:v",
                        "1",
                        "-vf",
                        vf,
                        "-f",
                        "rawvideo",
                        "-pix_fmt",
                        "rgb24",
                        "pipe:1",
                    ]
                )
                proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
                if proc.returncode != 0:
                    if hw_mode:
                        continue
                    continue
                if len(proc.stdout) >= expected_bytes:
                    return Image.frombytes(
                        "RGB", (self.strip_w, self.strip_h), proc.stdout[:expected_bytes]
                    )
        raise RuntimeError(
            f"Failed to decode raw strip for slot #{slot.slot_id} at {slot.mid_ms}ms"
        )

    def _load_single_pass_ppm(self, slot: TimingSlot) -> Image.Image | None:
        if self.single_pass_strips_dir is None:
            return None
        ppm_path = self.single_pass_strips_dir / f"strip_{slot.start_ms}_{slot.end_ms}.ppm"
        if not ppm_path.is_file():
            return None
        try:
            with Image.open(ppm_path) as im:
                img = im.convert("RGB")
        except Exception:
            return None
        finally:
            try:
                ppm_path.unlink(missing_ok=True)
            except Exception:
                pass

        if (
            self.resolved_region is not None
            and self.video_w > 0
            and self.resolved_region.x == 0
            and self.resolved_region.width >= self.video_w
        ):
            iw, ih = img.size
            tx = max(0, round(iw * 0.10))
            tw = max(1, min(iw - tx, round(iw * 0.80)))
            if tw < iw:
                img = img.crop((tx, 0, tx + tw, ih))

        iw, ih = img.size
        scale = min(self.strip_w / max(1, iw), self.strip_h / max(1, ih))
        new_w = max(1, min(self.strip_w, round(iw * scale)))
        new_h = max(1, min(self.strip_h, round(ih * scale)))
        if (iw, ih) != (new_w, new_h):
            img = img.resize((new_w, new_h), Image.Resampling.LANCZOS)

        if (new_w, new_h) != (self.strip_w, self.strip_h):
            canvas = Image.new("RGB", (self.strip_w, self.strip_h), (0, 0, 0))
            canvas.paste(img, ((self.strip_w - new_w) // 2, (self.strip_h - new_h) // 2))
            img = canvas

        with self._lock:
            self.single_pass_hits += 1
        return img

    def extract_strip_in_memory(self, slot: TimingSlot) -> Image.Image:
        """Consume single-pass exported strip if available, else decode via forward grab or seek."""
        ppm_img = self._load_single_pass_ppm(slot)
        if ppm_img is not None:
            return ppm_img

        cap_info = self._get_thread_capture()
        if cap_info is not None:
            cap, frame_dur_ms, cur_ms, max_grab_gap_ms = cap_info
            cv2 = self._cv2
            target_ms = float(slot.mid_ms)
            gap = target_ms - cur_ms

            ok = False
            frame = None
            used_grab = False
            if 0.0 <= gap <= max_grab_gap_ms:
                used_grab = True
                ok = True
                while cur_ms + frame_dur_ms * 0.6 < target_ms:
                    if not cap.grab():
                        ok = False
                        break
                    cur_ms = float(cap.get(cv2.CAP_PROP_POS_MSEC))
                if ok:
                    ok, frame = cap.retrieve()
                    if ok:
                        with self._lock:
                            self.cv2_grabs += 1

            if not ok or frame is None:
                cap.set(cv2.CAP_PROP_POS_MSEC, target_ms)
                ok, frame = cap.read()
                if ok and frame is not None:
                    cur_ms = float(cap.get(cv2.CAP_PROP_POS_MSEC))
                    with self._lock:
                        self.cv2_seeks += 1
                elif used_grab:
                    # Retry at start_ms if exact midpoint seek near EOF failed
                    cap.set(cv2.CAP_PROP_POS_MSEC, float(slot.start_ms))
                    ok, frame = cap.read()
                    if ok and frame is not None:
                        cur_ms = float(cap.get(cv2.CAP_PROP_POS_MSEC))
                        with self._lock:
                            self.cv2_seeks += 1

            if ok and frame is not None:
                self._set_thread_cur_ms(cur_ms)
                return self._crop_and_resize_bgr(frame)

        return self._extract_strip_ffmpeg_raw_pipe(slot)

    def prepare_sheet(self, sheet_index: int, batch_slots: list[TimingSlot]) -> PreparedSheet:
        """Decode a batch of midpoint strips on this thread and render PNG bytes in RAM."""
        if not batch_slots:
            raise ValueError("Cannot prepare an empty batch of TimingSlots")

        t_dec0 = time.perf_counter()
        strip_images: dict[int, Image.Image] = {}
        for slot in batch_slots:
            img = self.extract_strip_in_memory(slot)
            strip_images[slot.slot_id] = img
            if self.save_debug_strips_dir is not None:
                img.save(
                    self.save_debug_strips_dir / f"slot_{slot.slot_id:03d}.png",
                    format="PNG",
                    compress_level=1,
                )
        decode_s = time.perf_counter() - t_dec0

        t_enc0 = time.perf_counter()
        canvas = compose_contact_sheet_image(
            batch_slots,
            strip_images,
            sheet_index=sheet_index,
            strip_w=self.strip_w,
            strip_h=self.strip_h,
        )
        png_bytes = encode_image_png_bytes(canvas, compress_level=1)
        sheet_name = f"sheet_{sheet_index:02d}.png"
        if self.save_debug_sheets_dir is not None:
            sheet_file = self.save_debug_sheets_dir / sheet_name
            sheet_file.write_bytes(png_bytes)
            sheet_path = str(sheet_file)
        else:
            sheet_path = f"memory://{sheet_name}"
        encode_s = time.perf_counter() - t_enc0

        spec = SheetSpec(
            sheet_index=sheet_index,
            sheet_name=sheet_name,
            sheet_path=sheet_path,
            expected_slot_ids=[s.slot_id for s in batch_slots],
        )
        return PreparedSheet(
            spec=spec,
            png_bytes=png_bytes,
            batch_slots=list(batch_slots),
            strip_images=strip_images,
            decode_s=decode_s,
            encode_s=encode_s,
        )


def render_single_contact_sheet(
    batch: Iterable[TimingSlot],
    strip_paths_by_id: Mapping[int, Path] | None,
    out_path: Path,
    *,
    sheet_index: int,
    strip_w: int = 960,
    strip_h: int = 90,
    header_tag: str = "SubLift VLM OCR",
    strip_images_by_id: Mapping[int, Image.Image] | None = None,
) -> SheetSpec:
    """Render a single-column numbered Contact Sheet PNG and return its SheetSpec."""
    batch_list = list(batch)
    if not batch_list:
        raise ValueError("Cannot render an empty Contact Sheet")

    images: dict[int, Image.Image] = {}
    opened: list[Image.Image] = []
    try:
        for slot in batch_list:
            if strip_images_by_id is not None and slot.slot_id in strip_images_by_id:
                images[slot.slot_id] = strip_images_by_id[slot.slot_id]
            elif strip_paths_by_id is not None and slot.slot_id in strip_paths_by_id:
                raw_img = Image.open(strip_paths_by_id[slot.slot_id])
                opened.append(raw_img)
                images[slot.slot_id] = raw_img.convert("RGB")
            else:
                raise KeyError(f"Missing strip image/path for slot #{slot.slot_id}")

        canvas = compose_contact_sheet_image(
            batch_list,
            images,
            sheet_index=sheet_index,
            strip_w=strip_w,
            strip_h=strip_h,
            header_tag=header_tag,
        )
    finally:
        for img in opened:
            img.close()

    out_path.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(out_path, format="PNG", compress_level=1)
    return SheetSpec(
        sheet_index=sheet_index,
        sheet_name=out_path.name,
        sheet_path=str(out_path),
        expected_slot_ids=[s.slot_id for s in batch_list],
    )


def build_contact_sheets(
    slots: list[TimingSlot],
    strip_paths_by_id: dict[int, Path],
    sheet_dir: Path,
    *,
    slots_per_sheet: int = 10,
    strip_w: int = 960,
    strip_h: int = 90,
    quiet: bool = False,
) -> list[SheetSpec]:
    """Render all Contact Sheets and persist sheets_manifest.json."""
    if slots_per_sheet <= 0:
        raise ValueError(f"slots_per_sheet must be positive, got {slots_per_sheet}")

    t0 = time.perf_counter()
    sheet_dir.mkdir(parents=True, exist_ok=True)
    for old_png in sheet_dir.glob("sheet_*.png"):
        old_png.unlink()

    manifest: list[SheetSpec] = []
    for page_idx, start in enumerate(range(0, len(slots), slots_per_sheet), 1):
        batch = slots[start : start + slots_per_sheet]
        out_path = sheet_dir / f"sheet_{page_idx:02d}.png"
        spec = render_single_contact_sheet(
            batch,
            strip_paths_by_id,
            out_path,
            sheet_index=page_idx,
            strip_w=strip_w,
            strip_h=strip_h,
        )
        manifest.append(spec)

    manifest_doc = [
        {
            "sheet_index": s.sheet_index,
            "sheet_name": s.sheet_name,
            "sheet_path": s.sheet_path,
            "expected_slot_ids": s.expected_slot_ids,
        }
        for s in manifest
    ]
    (sheet_dir / "sheets_manifest.json").write_text(
        json.dumps(manifest_doc, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    _log(
        f"[Stage 2b] Rendered {len(manifest)} contact sheets in {time.perf_counter() - t0:.2f}s",
        quiet=quiet,
    )
    return manifest
