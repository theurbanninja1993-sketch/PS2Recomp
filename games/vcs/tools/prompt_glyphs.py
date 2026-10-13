#!/usr/bin/env python3
"""Shared prompt-glyph rendering for the pack builders.

The AL2009man face-button SVGs are white discs with low-contrast inner
symbols: at font/HUD sizes (~20-64 px) the inner shape disappears and only a
white circle remains. This module renders the *inner* art instead:

- xbox/steam/switch faces -> bare white letters (A/B/X/Y, positional),
  drawn as text so they stay crisp at any size;
- PlayStation faces -> the inner symbol of ps-Cross/Triangle/Square/Circle
  extracted from the disc as a white mask;
- d-pads -> the generic arrows (simple shapes; render fine as-is);
- keyboard -> white key labels.
"""
import os
import struct
import subprocess

from PIL import Image, ImageOps

FACE_LETTERS = {
    "xbox":      {"cross": "A", "circle": "B", "square": "X", "triangle": "Y"},
    "steam":     {"cross": "A", "circle": "B", "square": "X", "triangle": "Y"},
    # Nintendo layout: B bottom (cross position), A right, Y left, X top.
    "switch":    {"cross": "B", "circle": "A", "square": "Y", "triangle": "X"},
    "keyboard":  {"cross": "W", "circle": "LMB", "square": "S", "triangle": "E"},
}
PS_SVG = {
    "cross": "PlayStation/ps-Cross.svg",
    "circle": "PlayStation/ps-Circle.svg",
    "square": "PlayStation/ps-Square.svg",
    "triangle": "PlayStation/ps-Triangle.svg",
}
DPAD_SVG = {
    "dpad_up": "PlayStation/ps-D-PAD Up.svg",
    "dpad_down": "PlayStation/ps-D-PAD Down.svg",
    "dpad_left": "PlayStation/ps-D-PAD Left.svg",
    "dpad_right": "PlayStation/ps-D-PAD Right.svg",
}

_TMP = "/tmp/opencode/_prompt_glyph"


def _svg_to_png(svg_text: bytes, size: int, out: str):
    subprocess.run(["rsvg-convert", "-w", str(size), "-h", str(size), "-o", out, "-"],
                   input=svg_text, check=True)
    return out


def _text_glyph(label: str, size: int) -> Image.Image:
    os.makedirs(_TMP, exist_ok=True)
    fs = size if len(label) <= 1 else (int(size * 0.8) if len(label) == 2 else int(size * 0.6))
    big = size * 8
    svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{big}" height="{big}" viewBox="0 0 {big} {big}">'
           f'<text x="{big//2}" y="{int(big*0.70)}" font-family="DejaVu Sans, sans-serif" font-size="{fs*8}" '
           f'font-weight="bold" fill="#ffffff" text-anchor="middle">{label}</text></svg>')
    png = _svg_to_png(svg.encode(), big, f"{_TMP}/text.png")
    im = Image.open(png).convert("RGBA")
    return ImageOps.contain(im, (size, size), Image.LANCZOS)


def _ps_mask(svg_path: str, size: int) -> Image.Image:
    """The inner symbol of a disc-style PS glyph: pixels darker than the white
    disc become a white mask (the disc itself is dropped)."""
    os.makedirs(_TMP, exist_ok=True)
    big = max(128, size * 8)
    png = _svg_to_png(open(svg_path, "rb").read(), big, f"{_TMP}/ps.png")
    im = Image.open(png).convert("RGBA")
    px = im.load()
    w, h = im.size
    out = Image.new("RGBA", (w, h), (255, 255, 255, 0))
    opx = out.load()
    # central circle: keeps the disc rim/AA speckles out (the symbol always
    # sits well inside the disc)
    cx, cy = w / 2.0, h / 2.0
    r2 = (w * 0.42) ** 2
    for y in range(h):
        for x in range(w):
            if (x - cx) ** 2 + (y - cy) ** 2 > r2:
                continue
            r, g, b, a = px[x, y]
            if a < 60:
                continue
            lum = (r + g + b) // 3
            if lum < 205:  # the symbol strokes are darker than the disc
                alpha = min(255, max(0, (215 - lum) * 5))
                opx[x, y] = (255, 255, 255, alpha)
    return out


def render_prompt_glyph(set_name: str, button: str, size: int, svg_root: str) -> Image.Image:
    """Returns an RGBA tile of the button's prompt art at `size` px."""
    if set_name in ("xbox", "steam", "switch", "keyboard"):
        return _text_glyph(FACE_LETTERS[set_name][button], size)
    # dualsense / dualshock
    if button in PS_SVG:
        return _ps_mask(os.path.join(svg_root, PS_SVG[button]), size)
    return None
