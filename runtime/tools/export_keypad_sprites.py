#!/usr/bin/env python3
"""
Generate the default on-screen-keypad button PNGs that the runtime loads
from ``runtime/assets/keypad/default/<slug>.png``.

The runtime itself does not care about the PNG dimensions — it scales any
image into each button's rect at bake time. We pick 128×128 here so the
defaults stay sharp when scaled *down* to small finger-size buttons on
handhelds; users can drop in replacements at any size.

Usage:
    python3 runtime/tools/export_keypad_sprites.py [out_dir]

If ``out_dir`` is omitted it defaults to
``runtime/assets/keypad/default/`` relative to the repo root.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


# Colors: match the overlay's "Below" palette (see overlay.cpp kBtn*).
FILL        = (48, 48, 54, 255)
STROKE      = (150, 150, 160, 255)
LABEL_COLOR = (255, 255, 255, 255)

# Paths relative to this script (runtime/tools/).
HERE = Path(__file__).resolve().parent
RUNTIME_ROOT = HERE.parent
REPO_ROOT = RUNTIME_ROOT.parent

# Font paths — same TTFs the native runtime opens at render time.
TEXT_FONT_PATH = Path("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf")
ICON_FONT_PATH = RUNTIME_ROOT / "third_party" / "fontawesome" / "fa-solid-900.ttf"

# Font Awesome 6 Free Solid codepoints we reference. PUA characters; render
# them through the icon font. Keep the names + values lined up with the
# fa::k* constants in overlay.cpp.
FA = {
    "arrow-up":      "\uf062",
    "arrow-down":    "\uf063",
    "arrow-left":    "\uf060",
    "arrow-right":   "\uf061",
    "chevron-up":    "\uf077",
    "chevron-down":  "\uf078",
    "rotate":        "\uf021",
}


def make_button(w: int, h: int, label: str,
                font: ImageFont.FreeTypeFont) -> Image.Image:
    """Draw one button: filled body, 1-pixel stroke, glyph centered."""
    img = Image.new("RGBA", (w, h), FILL)
    draw = ImageDraw.Draw(img)
    # 1-px edge stroke, same color as the C++ overlay's kBtnBelowEdge.
    draw.rectangle([(0, 0), (w - 1, h - 1)], outline=STROKE, width=1)
    # Glyph centered using the font's real bbox (not the advance width) so
    # descenders / ascenders don't pull the visual off-center.
    bbox = draw.textbbox((0, 0), label, font=font)
    gx = (w - (bbox[2] - bbox[0])) // 2 - bbox[0]
    gy = (h - (bbox[3] - bbox[1])) // 2 - bbox[1]
    draw.text((gx, gy), label, font=font, fill=LABEL_COLOR)
    return img


def main() -> int:
    out_dir = Path(sys.argv[1]) if len(sys.argv) > 1 \
        else RUNTIME_ROOT / "assets" / "keypad" / "default"
    out_dir.mkdir(parents=True, exist_ok=True)

    size = 128
    text_font = ImageFont.truetype(str(TEXT_FONT_PATH), int(size * 0.55))
    icon_font = ImageFont.truetype(str(ICON_FONT_PATH), int(size * 0.55))

    # (slug, w, h, glyph, font). Slugs match the labels referenced by
    # the JSON layouts in runtime/assets/keypad/layouts/. Most buttons
    # are square; "start" is exported 2:1 because the default Minimal
    # layout gives it a wider rect and we want the text to fill it
    # without aspect-preserving bands on the sides.
    diag_font  = ImageFont.truetype(str(TEXT_FONT_PATH), int(size * 0.6))
    start_font = ImageFont.truetype(str(TEXT_FONT_PATH), int(size * 0.45))
    jobs = [
        ("arrow-up",         size,     size, FA["arrow-up"],    icon_font),
        ("arrow-down",       size,     size, FA["arrow-down"],  icon_font),
        ("arrow-left",       size,     size, FA["arrow-left"],  icon_font),
        ("arrow-right",      size,     size, FA["arrow-right"], icon_font),
        ("arrow-up-left",    size,     size, "\u2196",          diag_font),
        ("arrow-up-right",   size,     size, "\u2197",          diag_font),
        ("arrow-down-right", size,     size, "\u2198",          diag_font),
        ("arrow-down-left",  size,     size, "\u2199",          diag_font),
        ("start",            size * 2, size, "Start",           start_font),
        ("soft-left",        size,     size, "A",               text_font),
        ("soft-right",       size,     size, "B",               text_font),
        ("asterisk",         size,     size, "*",               text_font),
        ("pound",            size,     size, "#",               text_font),
    ]
    for digit in "0123456789":
        jobs.append((f"num-{digit}", size, size, digit, text_font))

    # Control icons — toggle chevrons + layout-cycle. Always square.
    jobs += [
        ("toggle-hide", size, size, FA["chevron-up"],   icon_font),
        ("toggle-show", size, size, FA["chevron-down"], icon_font),
        ("cycle",       size, size, FA["rotate"],       icon_font),
    ]

    for slug, w, h, glyph, font in jobs:
        img = make_button(w, h, glyph, font)
        path = out_dir / f"{slug}.png"
        img.save(path)
        print(f"  wrote {path.relative_to(REPO_ROOT)}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
