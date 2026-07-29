"""
scripts/parity/gen_paddle_fixtures.py
-------------------------------------
Generate synthetic clean, offline PNG image fixtures for Paddle stage parity testing.
Saves PNG files into benchmark/parity/fixtures/paddle/.
"""

from __future__ import annotations

import json
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


def create_fixture_image(
    text_lines: list[tuple[str, tuple[int, int], int]],
    size: tuple[int, int] = (800, 200),
    bg_color: tuple[int, int, int] = (20, 20, 20),
    text_color: tuple[int, int, int] = (240, 240, 240),
) -> Image.Image:
    """Create a synthetic RGB image with specified text lines."""
    img = Image.new("RGB", size, color=bg_color)
    draw = ImageDraw.Draw(img)

    try:
        font = ImageFont.truetype("/System/Library/Fonts/PingFang.ttc", 28)
    except Exception:
        font = ImageFont.load_default()

    for text, pos, font_size in text_lines:
        try:
            curr_font = ImageFont.truetype("/System/Library/Fonts/PingFang.ttc", font_size)
        except Exception:
            curr_font = font
        draw.text(pos, text, fill=text_color, font=curr_font)

    return img


def generate_all_fixtures() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    out_dir = repo_root / "benchmark" / "parity" / "fixtures" / "paddle"
    out_dir.mkdir(parents=True, exist_ok=True)

    fixtures = [
        ("empty.png", []),
        ("cjk_single_line.png", [("SubLift 硬字幕提取引擎", (40, 60), 32)]),
        ("latin_single_line.png", [("SubLift Hard Subtitle Extractor", (40, 60), 30)]),
        ("mixed_cjk_latin.png", [("SubLift v2.0 - 自动提取 100% 精度!", (40, 60), 28)]),
        ("stroke_outlined.png", [("SubLift 高清晰度硬字幕", (40, 60), 32)]),
        (
            "tilted_adjacent.png",
            [("第一行: 顶级检测框", (40, 30), 26), ("第二行: 底部精准对齐", (40, 100), 26)],
        ),
    ]

    manifest = []
    for name, lines in fixtures:
        img_path = out_dir / name
        img = create_fixture_image(lines)
        img.save(img_path)
        manifest.append({
            "name": name,
            "width": img.width,
            "height": img.height,
            "text_count": len(lines),
        })

    meta_path = out_dir / "manifest.json"
    content = json.dumps({"fixtures": manifest}, indent=2, ensure_ascii=False) + "\n"
    meta_path.write_text(content, encoding="utf-8")
    print(f"[OK] Generated {len(fixtures)} paddle fixtures in {out_dir}")


if __name__ == "__main__":
    generate_all_fixtures()
