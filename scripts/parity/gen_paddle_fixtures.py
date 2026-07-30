"""Generate committed, offline Paddle stage-parity fixtures.

Regeneration is explicit because font rasterization is platform-sensitive.
The committed PNG hashes, rather than this script, are the cross-platform
contract consumed by default checks.
"""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
from typing import Any

from PIL import Image, ImageDraw, ImageFilter, ImageFont

CANVAS_SIZE = (960, 256)
BACKGROUND = (18, 20, 24)


def _font_path() -> Path:
    candidates = [
        os.environ.get("SUBLIFT_CJK_FONT"),
        "/System/Library/Fonts/Hiragino Sans GB.ttc",
        "/System/Library/Fonts/STHeiti Medium.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
    ]
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return Path(candidate)
    raise RuntimeError(
        "No deterministic CJK font found. Set SUBLIFT_CJK_FONT before "
        "regenerating Paddle fixtures."
    )


def _font(path: Path, size: int) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(str(path), size)


def _base() -> Image.Image:
    return Image.new("RGB", CANVAS_SIZE, BACKGROUND)


def _draw_centered(
    image: Image.Image,
    text: str,
    *,
    font: ImageFont.FreeTypeFont,
    y: int,
    fill: tuple[int, int, int] = (242, 244, 248),
    stroke_width: int = 0,
    stroke_fill: tuple[int, int, int] = (0, 0, 0),
) -> None:
    draw = ImageDraw.Draw(image)
    bounds = draw.textbbox((0, 0), text, font=font, stroke_width=stroke_width)
    width = bounds[2] - bounds[0]
    x = (image.width - width) // 2
    draw.text(
        (x, y),
        text,
        font=font,
        fill=fill,
        stroke_width=stroke_width,
        stroke_fill=stroke_fill,
    )


def _tilted(text: str, font: ImageFont.FreeTypeFont) -> Image.Image:
    layer = Image.new("RGBA", (760, 100), (0, 0, 0, 0))
    draw = ImageDraw.Draw(layer)
    draw.text(
        (20, 16),
        text,
        font=font,
        fill=(246, 246, 246, 255),
        stroke_width=1,
        stroke_fill=(0, 0, 0, 255),
    )
    rotated = layer.rotate(
        7.0,
        resample=Image.Resampling.BICUBIC,
        expand=True,
        fillcolor=(0, 0, 0, 0),
    )
    image = _base()
    image.paste(
        rotated,
        ((image.width - rotated.width) // 2, 70),
        rotated,
    )
    return image


def _rotated_180(text: str, font: ImageFont.FreeTypeFont) -> Image.Image:
    image = _base()
    _draw_centered(image, text, font=font, y=92)
    return image.rotate(180, resample=Image.Resampling.BICUBIC)


def _sha256(path: Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def generate_all_fixtures() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    out_dir = repo_root / "benchmark" / "parity" / "fixtures" / "paddle"
    out_dir.mkdir(parents=True, exist_ok=True)

    font_path = _font_path()
    regular = _font(font_path, 42)
    small = _font(font_path, 36)

    fixtures: list[tuple[str, Image.Image, list[str], list[str]]] = []

    fixtures.append(("empty.png", _base(), [], ["empty"]))

    cjk = _base()
    _draw_centered(cjk, "硬字幕提取测试", font=regular, y=92)
    fixtures.append(
        ("cjk_single_line.png", cjk, ["硬字幕提取测试"], ["cjk", "single-line"])
    )

    latin = _base()
    _draw_centered(latin, "SubLift Hard Subtitle Extractor", font=small, y=94)
    fixtures.append(
        (
            "latin_single_line.png",
            latin,
            ["SubLift Hard Subtitle Extractor"],
            ["latin", "single-line"],
        )
    )

    mixed = _base()
    _draw_centered(mixed, "SubLift 自动提取 100%", font=regular, y=92)
    fixtures.append(
        (
            "mixed_cjk_latin.png",
            mixed,
            ["SubLift 自动提取 100%"],
            ["mixed", "single-line"],
        )
    )

    outlined = _base()
    _draw_centered(
        outlined,
        "描边字幕质量测试",
        font=regular,
        y=92,
        fill=(255, 244, 90),
        stroke_width=3,
        stroke_fill=(10, 10, 10),
    )
    fixtures.append(
        (
            "stroke_outlined.png",
            outlined,
            ["描边字幕质量测试"],
            ["cjk", "outline", "color"],
        )
    )

    low_contrast = Image.new("RGB", CANVAS_SIZE, (72, 74, 78))
    _draw_centered(
        low_contrast,
        "低对比度字幕",
        font=regular,
        y=92,
        fill=(146, 148, 152),
    )
    low_contrast = low_contrast.filter(ImageFilter.GaussianBlur(radius=0.7))
    fixtures.append(
        (
            "low_contrast_blur.png",
            low_contrast,
            ["低对比度字幕"],
            ["cjk", "low-contrast", "blur"],
        )
    )

    fixtures.append(
        (
            "tilted_single_line.png",
            _tilted("轻微倾斜字幕", regular),
            ["轻微倾斜字幕"],
            ["cjk", "tilted"],
        )
    )

    adjacent = _base()
    _draw_centered(adjacent, "第一行相邻字幕", font=small, y=54)
    _draw_centered(adjacent, "Second line 第二行", font=small, y=132)
    fixtures.append(
        (
            "adjacent_two_line.png",
            adjacent,
            ["第一行相邻字幕", "Second line 第二行"],
            ["mixed", "two-line", "adjacent"],
        )
    )

    fixtures.append(
        (
            "rotated_180.png",
            _rotated_180("方向分类测试", regular),
            ["方向分类测试"],
            ["cjk", "rotated-180", "cls"],
        )
    )

    manifest: list[dict[str, Any]] = []
    keep_names = {name for name, _image, _text, _tags in fixtures}
    for stale in out_dir.glob("*.png"):
        if stale.name not in keep_names:
            stale.unlink()

    for name, image, expected_text, tags in fixtures:
        image_path = out_dir / name
        image.save(image_path, format="PNG", optimize=False)
        manifest.append(
            {
                "name": name,
                "width": image.width,
                "height": image.height,
                "expected_text": expected_text,
                "tags": tags,
                "sha256": _sha256(image_path),
            }
        )

    metadata = {
        "schema_version": 2,
        "font_family": "Hiragino Sans GB",
        "font_asset_note": (
            "PNG files and hashes are authoritative; regeneration requires "
            "an explicitly supported CJK font."
        ),
        "fixtures": manifest,
    }
    (out_dir / "manifest.json").write_text(
        json.dumps(metadata, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    print(f"[OK] Generated {len(fixtures)} Paddle fixtures in {out_dir}")


if __name__ == "__main__":
    generate_all_fixtures()
