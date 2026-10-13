#!/usr/bin/env python3
"""Shared prompt-glyph rendering for the pack builders.

Renders each set's prompts in its console's modern style - a dark disc with
the button's letter/symbol (the disc is the "ring" the game's originals have,
and gives the contrast these sizes need):

- PlayStation (DualSense/DualShock): the pack's Color SVGs as-is (dark disc,
  white shape, coloured accents).
- Xbox: dark disc + the letter in its official colour (A green, B red,
  X blue, Y yellow).
- Switch: dark disc + white letter (positional B/A/Y/X) + red ring.
- Steam: dark disc + white letter + Steam-blue ring.
- Keyboard: dark key + white label (the port's default bindings).

D-pads use the generic arrows (simple shapes that read at any size).
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
# letter colours (Xbox official-ish); other sets use white letters.
LETTER_COLORS = {
    "xbox": {"A": (16, 124, 16), "B": (211, 47, 47), "X": (0, 120, 215), "Y": (242, 196, 0)},
}
RING_COLORS = {
    "xbox": None,                       # the coloured letter carries the identity
    "steam": (102, 192, 244),           # Steam blue
    "switch": (230, 0, 18),             # Nintendo red
    "keyboard": (170, 170, 170),
}
PS_SVG = {
    "cross": "PlayStation/ps-Cross-Color.svg",
    "circle": "PlayStation/ps-Circle-Color.svg",
    "square": "PlayStation/ps-Square-Color.svg",
    "triangle": "PlayStation/ps-Triangle-Color.svg",
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


def _disc_glyph(label: str, size: int, letter_color, ring_color) -> Image.Image:
    ss = 4  # supersample for round edges
    big = size * ss
    im = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    pad = max(2, big // 16)
    # dark disc
    d.ellipse([pad, pad, big - pad - 1, big - pad - 1], fill=(28, 28, 32, 255))
    # ring
    if ring_color:
        w = max(ss, big // 12)
        d.ellipse([pad, pad, big - pad - 1, big - pad - 1], outline=(*ring_color, 255), width=w)
    # letter
    fs = int(big * (0.52 if len(label) == 1 else (0.42 if len(label) == 3 else 0.5)))
    font = _font(fs)
    bbox = d.textbbox((0, 0), label, font=font)
    tx = (big - (bbox[2] - bbox[0])) // 2 - bbox[0]
    ty = int(big * 0.5) - (bbox[3] - bbox[1]) // 2 - bbox[1]
    d.text((tx, ty), label, font=font, fill=(*letter_color, 255))
    return im.resize((size, size), Image.LANCZOS)


def render_prompt_glyph(set_name: str, button: str, size: int, svg_root: str) -> Image.Image:
    """Returns an RGBA tile of the button's prompt art at `size` px."""
    if button in PS_SVG and set_name in ("dualsense", "dualshock"):
        os.makedirs(_TMP, exist_ok=True)
        big = max(128, size * 4)
        png = f"{_TMP}/ps.png"
        subprocess.run(["rsvg-convert", "-w", str(big), "-h", str(big), "-o", png, "-"],
                       input=open(os.path.join(svg_root, PS_SVG[button]), "rb").read(), check=True)
        im = Image.open(png).convert("RGBA")
        return im.resize((size, size), Image.LANCZOS)
    if set_name in FACE_LETTERS and button in FACE_LETTERS[set_name]:
        label = FACE_LETTERS[set_name][button]
        letter = LETTER_COLORS.get(set_name, {}).get(label, (245, 245, 245))
        return _disc_glyph(label, size, letter, RING_COLORS.get(set_name))
    return None
