#!/usr/bin/env python3
"""Shared prompt-glyph rendering for the pack builders.

Renders each set's prompts as a light disc with the button's coloured
letter/symbol. The game's UI is dark (HUD help boxes, controller page), so the
art itself must be light to read there - and a white disc with coloured marks
also reads on the menus' light panels:

- PlayStation (DualSense/DualShock): white disc + the shape in its brand
  colour (cross blue, circle red, square pink, triangle green).
- Xbox: white disc + the letter in its official colour (A green, B red,
  X blue, Y yellow).
- Switch: white disc + red letter (positional B/A/Y/X) + red ring.
- Steam: white disc + blue letter + Steam-blue ring.
- Keyboard: white disc + dark label (the port's default bindings).

D-pads use the generic arrows (already light solid shapes).
"""
import os
import subprocess

from PIL import Image, ImageDraw, ImageFont

FACE_LETTERS = {
    "xbox":      {"cross": "A", "circle": "B", "square": "X", "triangle": "Y"},
    "steam":     {"cross": "A", "circle": "B", "square": "X", "triangle": "Y"},
    # Nintendo layout: B bottom (cross position), A right, Y left, X top.
    "switch":    {"cross": "B", "circle": "A", "square": "Y", "triangle": "X"},
    "keyboard":  {"cross": "W", "circle": "LMB", "square": "S", "triangle": "E"},
}
LETTER_COLORS = {
    "xbox": {"A": (16, 124, 16), "B": (211, 47, 47), "X": (0, 120, 215), "Y": (200, 152, 0)},
    "switch": {"A": (230, 0, 18), "B": (230, 0, 18), "X": (230, 0, 18), "Y": (230, 0, 18)},
    "steam": {"A": (30, 110, 160), "B": (30, 110, 160), "X": (30, 110, 160), "Y": (30, 110, 160)},
    "keyboard": {"W": (40, 40, 45), "S": (40, 40, 45), "E": (40, 40, 45), "LMB": (40, 40, 45)},
}
RING_COLORS = {
    "xbox": (120, 120, 120),
    "steam": (102, 192, 244),
    "switch": (230, 0, 18),
    "keyboard": (150, 150, 150),
}
# PlayStation brand colours for the shapes.
PS_COLOR = {
    "cross": (59, 130, 246),
    "circle": (239, 68, 68),
    "square": (236, 72, 153),
    "triangle": (16, 163, 90),
}
PS_WHITE_SVG = {
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


def _font(size: int) -> ImageFont.FreeTypeFont:
    for path in ("/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
                 "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
                 "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"):
        if os.path.exists(path):
            return ImageFont.truetype(path, size)
    return ImageFont.load_default()


def _disc(canvas_px: int, fill=(242, 242, 246, 255), ring=None, ring_w=0) -> Image.Image:
    im = Image.new("RGBA", (canvas_px, canvas_px), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    pad = max(2, canvas_px // 16)
    d.ellipse([pad, pad, canvas_px - pad - 1, canvas_px - pad - 1], fill=fill)
    if ring and ring_w:
        d.ellipse([pad, pad, canvas_px - pad - 1, canvas_px - pad - 1], outline=(*ring, 255), width=ring_w)
    return im


def _disc_letter(label: str, size: int, letter_color, ring_color) -> Image.Image:
    ss = 4
    big = size * ss
    im = _disc(big, ring=ring_color, ring_w=max(ss, big // 14))
    d = ImageDraw.Draw(im)
    fs = int(big * (0.52 if len(label) == 1 else (0.42 if len(label) == 3 else 0.5)))
    font = _font(fs)
    bbox = d.textbbox((0, 0), label, font=font)
    tx = (big - (bbox[2] - bbox[0])) // 2 - bbox[0]
    ty = int(big * 0.5) - (bbox[3] - bbox[1]) // 2 - bbox[1]
    d.text((tx, ty), label, font=font, fill=(*letter_color, 255))
    return im.resize((size, size), Image.LANCZOS)


def _disc_ps(shape: str, size: int, svg_root: str) -> Image.Image:
    """White disc + the shape's mask from the white SVG, drawn brand-coloured."""
    os.makedirs(_TMP, exist_ok=True)
    ss = 4
    big = max(128, size * ss)
    png = f"{_TMP}/pswhite.png"
    subprocess.run(["rsvg-convert", "-w", str(big), "-h", str(big), "-o", png, "-"],
                   input=open(os.path.join(svg_root, PS_WHITE_SVG[shape]), "rb").read(), check=True)
    src = Image.open(png).convert("RGBA")
    px = src.load()
    w, h = src.size
    disc = _disc(big, ring=PS_COLOR[shape], ring_w=max(ss, big // 16))
    dpx = disc.load()
    color = PS_COLOR[shape]
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
            if lum < 210:  # the strokes are darker than the white disc
                alpha = min(255, max(0, (215 - lum) * 5))
                base = dpx[x, y]
                mix = alpha / 255.0
                dpx[x, y] = (int(color[0] * mix + base[0] * (1 - mix)),
                             int(color[1] * mix + base[1] * (1 - mix)),
                             int(color[2] * mix + base[2] * (1 - mix)), 255)
    return disc.resize((size, size), Image.LANCZOS)


def render_prompt_glyph(set_name: str, button: str, size: int, svg_root: str) -> Image.Image:
    """Returns an RGBA tile of the button's prompt art at `size` px."""
    if button in PS_WHITE_SVG and set_name in ("dualsense", "dualshock"):
        return _disc_ps(button, size, svg_root)
    if set_name in FACE_LETTERS and button in FACE_LETTERS[set_name]:
        label = FACE_LETTERS[set_name][button]
        letter = LETTER_COLORS.get(set_name, {}).get(label, (40, 40, 45))
        return _disc_letter(label, size, letter, RING_COLORS.get(set_name))
    return None
