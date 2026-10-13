#!/usr/bin/env python3
"""Shared prompt-glyph rendering for the pack builders.

The approved look (reads on the light menus and panels):
- PlayStation DualShock: the pack's Color SVGs (dark disc, coloured shape,
  like the real DS4); DualSense: the pack's white style (white disc, gray
  symbol, like the white PS buttons).
- Xbox: dark disc + the letter in its official colour (A green, B red, X blue,
  Y yellow).
- Switch: dark disc + white letter (positional B/A/Y/X) + red ring.
- Steam: dark disc + white letter + Steam-blue ring.
- Keyboard: keycap squares with the bound key label (W/LMB/S/E; the
  d-pad/menu directions are the arrow keys).

D-pads use the generic arrows (simple light shapes).
"""
import os
import re
import subprocess

from PIL import Image, ImageDraw, ImageFont

FACE_LETTERS = {
    "xbox":      {"cross": "A", "circle": "B", "square": "X", "triangle": "Y"},
    "steam":     {"cross": "A", "circle": "B", "square": "X", "triangle": "Y"},
    # Nintendo layout: B bottom (cross position), A right, Y left, X top.
    "switch":    {"cross": "B", "circle": "A", "square": "Y", "triangle": "X"},
}
# Keyboard: keycap squares labelled with the port's default bindings. The
# d-pad / menu directions are the arrow keys. The map-screen icons map the
# PS2 buttons to the action that presses them on that screen (Square is the
# menu-context jump, L1/R1 are interact / aim on foot).
KEYBOARD_KEYS = {
    "cross": "W", "circle": "LMB", "square": "S", "triangle": "E",
    "dpad_up": "\u2191", "dpad_down": "\u2193",
    "dpad_left": "\u2190", "dpad_right": "\u2192",
    "start": "ESC", "select": "TAB",
    "map_target": "SPACE", "map_zoom_l": "Q", "map_zoom_r": "RMB",
}
# Shoulder (map zoom) pills for the console sets. The DualSense uses the
# pack's own bumper glyph (grayed to the controller's tone) instead; the
# DualShock's real bumpers are dark with light print, so it gets the dark
# pill style like the Xbox/Steam/Switch sets.
SHOULDER_LABELS = {
    "xbox": {"map_zoom_l": "LB", "map_zoom_r": "RB"},
    "steam": {"map_zoom_l": "LB", "map_zoom_r": "RB"},
    "switch": {"map_zoom_l": "L", "map_zoom_r": "R"},
    "dualshock": {"map_zoom_l": "L1", "map_zoom_r": "R1"},
}
LETTER_COLORS = {
    "xbox": {"A": (16, 124, 16), "B": (211, 47, 47), "X": (0, 120, 215), "Y": (242, 196, 0)},
}
RING_COLORS = {
    "xbox": None,
    "steam": (102, 192, 244),   # Steam blue
    "switch": (230, 0, 18),     # Nintendo red
}
PS_COLOR_SVG = {
    "cross": "PlayStation/ps-Cross-Color.svg",
    "circle": "PlayStation/ps-Circle-Color.svg",
    "square": "PlayStation/ps-Square-Color.svg",
    "triangle": "PlayStation/ps-Triangle-Color.svg",
}
# The pack's white style: white disc + gray (#808080) symbol, meant to work
# on any background (DualSense buttons are white-symboled, not coloured).
PS_WHITE_SVG = {
    "cross": "PlayStation/ps-Cross.svg",
    "circle": "PlayStation/ps-Circle.svg",
    "square": "PlayStation/ps-Square.svg",
    "triangle": "PlayStation/ps-Triangle.svg",
}
# Shoulder prompts for the DualSense: the pack's bumper shape (the game's own
# PS2 shoulder icons have a different silhouette). The pack draws it
# near-white; the set darkens it to the controller's gray bumper tone.
PS_SHOULDER_SVG = {
    "map_zoom_l": "PlayStation/ps5-Left Back.svg",
    "map_zoom_r": "PlayStation/ps5-Right Back.svg",
}
PS_SHOULDER_BRIGHTNESS = {
    "dualsense": 0.72,
}
DPAD_SVG = {
    "dpad_up": "PlayStation/ps-D-PAD Up.svg",
    "dpad_down": "PlayStation/ps-D-PAD Down.svg",
    "dpad_left": "PlayStation/ps-D-PAD Left.svg",
    "dpad_right": "PlayStation/ps-D-PAD Right.svg",
}

_TMP = "/tmp/opencode/_prompt_glyph"


def to_gs_alpha(im: Image.Image) -> Image.Image:
    """Rescale 0..255 alpha art to the GS-scale alpha the runtime expects.

    Replacement textures sample through the GS blend emulation, where the
    blend factor is k = alpha/128 (0x80 = opaque). Art authored with
    255-alpha makes k ~ 2: the source is doubled and the destination
    factor goes negative, so prompts render over-bright and see-through.
    Dumped textures and the DDS converter already use the GS scale.
    """
    out = im.copy()
    a = out.getchannel("A").point(lambda v: (v * 128 + 127) // 255)
    out.putalpha(a)
    return out


def _font(size: int) -> ImageFont.FreeTypeFont:
    for path in ("/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
                 "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
                 "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"):
        if os.path.exists(path):
            return ImageFont.truetype(path, size)
    return ImageFont.load_default()


def _disc_glyph(label: str, size: int, letter_color, ring_color) -> Image.Image:
    ss = 4
    big = size * ss
    im = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    pad = max(2, big // 16)
    d.ellipse([pad, pad, big - pad - 1, big - pad - 1], fill=(28, 28, 32, 255))
    if ring_color:
        w = max(ss, big // 12)
        d.ellipse([pad, pad, big - pad - 1, big - pad - 1], outline=(*ring_color, 255), width=w)
    fs = int(big * (0.52 if len(label) == 1 else (0.42 if len(label) == 3 else 0.5)))
    font = _font(fs)
    bbox = d.textbbox((0, 0), label, font=font)
    tx = (big - (bbox[2] - bbox[0])) // 2 - bbox[0]
    ty = int(big * 0.5) - (bbox[3] - bbox[1]) // 2 - bbox[1]
    d.text((tx, ty), label, font=font, fill=(*letter_color, 255))
    return im.resize((size, size), Image.LANCZOS)


_STYLE_GROUP_RE = re.compile(r'(inkscape:label="Style[^"]*")(\s+)(style="[^"]*")')


def _svg_with_style(svg: bytes, style: str) -> bytes:
    """Select one of the pack's embedded style groups.

    Every prompt SVG ships three variants as Inkscape groups - "Style -
    Transparent", "Style - Outline (Black)" and "Style - Outline (White)" -
    with one visible. rsvg draws whichever group is inline, so make the
    choice explicit: "Transparent" is the white-button/gray-symbol look.
    """
    def repl(m):
        label, gap, st = m.groups()
        want = style in label
        st = re.sub(r"display:(none|inline)", "display:inline" if want else "display:none", st)
        return f"{label}{gap}{st}"
    return _STYLE_GROUP_RE.sub(repl, svg.decode("utf-8")).encode("utf-8")


def _keycap_glyph(label: str, size: int) -> Image.Image:
    """Keyboard prompts: a rounded-square keycap with the binding label."""
    ss = 4
    big = size * ss
    im = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    pad = max(2, big // 16)
    d.rounded_rectangle([pad, pad, big - pad - 1, big - pad - 1],
                        radius=max(4, big // 6),
                        fill=(32, 38, 46, 255), outline=(138, 148, 166, 255),
                        width=max(ss, big // 16))
    frac = 0.55 if len(label) == 1 else (0.45 if len(label) == 2 else 0.34)
    font = _font(int(big * frac))
    bbox = d.textbbox((0, 0), label, font=font)
    target = (big - 2 * pad) * 0.78
    if bbox[2] - bbox[0] > target:
        fs = max(8, int(int(big * frac) * target / (bbox[2] - bbox[0])))
        font = _font(fs)
        bbox = d.textbbox((0, 0), label, font=font)
    tx = (big - (bbox[2] - bbox[0])) // 2 - bbox[0]
    ty = int(big * 0.5) - (bbox[3] - bbox[1]) // 2 - bbox[1]
    d.text((tx, ty), label, font=font, fill=(232, 236, 242, 255))
    return im.resize((size, size), Image.LANCZOS)


def _shoulder_glyph(label: str, size: int) -> Image.Image:
    """Shoulder / bumper prompts: a wide pill with the button label."""
    ss = 4
    big = size * ss
    im = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    pad = max(2, big // 16)
    top = int(big * 0.28)
    bot = big - top - 1
    d.rounded_rectangle([pad, top, big - pad - 1, bot], radius=(bot - top) // 2,
                        fill=(32, 38, 46, 255), outline=(138, 148, 166, 255),
                        width=max(ss, big // 16))
    fs = int((bot - top) * 0.78)
    font = _font(fs)
    bbox = d.textbbox((0, 0), label, font=font)
    target = (big - 2 * pad) * 0.7
    if bbox[2] - bbox[0] > target:
        fs = max(8, int(fs * target / (bbox[2] - bbox[0])))
        font = _font(fs)
        bbox = d.textbbox((0, 0), label, font=font)
    tx = (big - (bbox[2] - bbox[0])) // 2 - bbox[0]
    ty = (top + bot) // 2 - (bbox[3] - bbox[1]) // 2 - bbox[1]
    d.text((tx, ty), label, font=font, fill=(232, 236, 242, 255))
    return im.resize((size, size), Image.LANCZOS)


def _render_ps_svg(svg_root: str, rel: str, size: int) -> Image.Image:
    """Rasterise one of the pack's PlayStation SVGs in its default
    ('Transparent') style group, at 4x and downscaled."""
    os.makedirs(_TMP, exist_ok=True)
    big = max(128, size * 4)
    png = f"{_TMP}/ps.png"
    raw = _svg_with_style(open(os.path.join(svg_root, rel), "rb").read(), "Transparent")
    subprocess.run(["rsvg-convert", "-w", str(big), "-h", str(big), "-o", png, "-"],
                   input=raw, check=True)
    return Image.open(png).convert("RGBA").resize((size, size), Image.LANCZOS)


def render_prompt_glyph(set_name: str, button: str, size: int, svg_root: str) -> Image.Image:
    """Returns an RGBA tile of the button's prompt art at `size` px."""
    if set_name == "keyboard":
        return _keycap_glyph(KEYBOARD_KEYS[button], size) if button in KEYBOARD_KEYS else None
    if button in SHOULDER_LABELS.get(set_name, {}):
        return _shoulder_glyph(SHOULDER_LABELS[set_name][button], size)
    if set_name == "dualsense" and button in PS_SHOULDER_SVG:
        im = _render_ps_svg(svg_root, PS_SHOULDER_SVG[button], size)
        factor = PS_SHOULDER_BRIGHTNESS.get(set_name, 1.0)
        if factor != 1.0:
            from PIL import ImageEnhance
            im = ImageEnhance.Brightness(im).enhance(factor)  # RGB only; alpha kept
        return im
    if button == "map_target":
        button = "square"  # the map's target toggle is the square button
    if button in PS_COLOR_SVG and set_name in ("dualsense", "dualshock"):
        # DualShock: the pack's coloured symbols (like the real DS4).
        # DualSense: the pack's white style (white disc, gray symbol).
        svg = PS_WHITE_SVG[button] if set_name == "dualsense" else PS_COLOR_SVG[button]
        return _render_ps_svg(svg_root, svg, size)
    if set_name in FACE_LETTERS and button in FACE_LETTERS[set_name]:
        label = FACE_LETTERS[set_name][button]
        letter = LETTER_COLORS.get(set_name, {}).get(label, (245, 245, 245))
        return _disc_glyph(label, size, letter, RING_COLORS.get(set_name))
    return None
