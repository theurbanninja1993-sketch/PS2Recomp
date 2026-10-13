#!/usr/bin/env python3
"""Redraw the button-symbol cells of the game's font/UI atlas per prompt set.

The tutorial prompts (Circle = melee, Square = brake/reverse, ...) are font
glyphs inside the 256x256 T8 atlas at tbp0=13592 (key
3ece7b57667e958b-f663bcea3bca5023-00002213.png as dumped):
  square   (0,230)-(23,251)   disc with a square outline
  cross    (24,230)-(45,251)  disc with an X
  circle   (46,230)-(67,251)  plain disc
  triangle (212,206)-(233,227) disc with a triangle

Everything else in the atlas stays byte-identical, so the rest of the text is
untouched; the game keeps sampling the CLUT for it (the replacement only
bypasses the palette inside the redrawn cells).

Usage:
  make_font_sheet.py --svg <Gamepad-Prompt-Asset-Pack dir> \
      --base <dumped 3ece...png> --out <prompts dir>
Writes prompts/<set>/3ece7b57667e958b-f663bcea3bca5023-00002213.png for
xbox/steam/dualsense/dualshock/switch/keyboard.
"""
import argparse, os, subprocess, struct, sys
from PIL import Image, ImageOps, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from prompt_glyphs import render_prompt_glyph

SHEET_KEY = "3ece7b57667e958b-f663bcea3bca5023-00002213.png"
CELLS = [
    (0, 230, 24, 22, "square"),
    (24, 230, 22, 22, "cross"),
    (46, 230, 22, 22, "circle"),
    (212, 206, 22, 22, "triangle"),
]

SVG = {
    "xbox":      {"cross": "Xbox/xbox-P1.svg", "circle": "Xbox/xbox-P2.svg",
                  "square": "Xbox/xbox-P3.svg", "triangle": "Xbox/xbox-P4.svg"},
    "steam":     {"cross": "Xbox/xbox-P1.svg", "circle": "Xbox/xbox-P2.svg",
                  "square": "Xbox/xbox-P3.svg", "triangle": "Xbox/xbox-P4.svg"},
    "dualsense": {"cross": "PlayStation/ps-Cross.svg", "circle": "PlayStation/ps-Circle.svg",
                  "square": "PlayStation/ps-Square.svg", "triangle": "PlayStation/ps-Triangle.svg"},
    "dualshock": {"cross": "PlayStation/ps-Cross.svg", "circle": "PlayStation/ps-Circle.svg",
                  "square": "PlayStation/ps-Square.svg", "triangle": "PlayStation/ps-Triangle.svg"},
}
# Switch positional layout (B bottom, A right, Y left, X top) from the Pro
# controller face-button sheet: 2x2, A = bottom-right.
SWITCH_SHEET = ("sheet:Controller Asset Pack/Nintendo Switch Controller Images/Switch Pro Controller/"
                "Default Theme/Theme Assets/Active Presses/NSwitchPro_FaceButton.png")
SWITCH_CELL = {"cross": "0,1", "circle": "1,1", "square": "0,0", "triangle": "1,0"}
# Keyboard: the port's default bindings for the action each symbol usually
# means in prompts (W accelerate / LMB attack / S brake-reverse / E vehicle).
KEYBOARD = {"cross": "W", "circle": "LMB", "square": "S", "triangle": "E"}


def render_glyph(set_name, button, size, svg_root, pack_root):
    tmp = f"/tmp/opencode/_fontglyph_{set_name}_{button}.png"
    os.makedirs("/tmp/opencode", exist_ok=True)
    if set_name == "keyboard":
        label = KEYBOARD[button]
        fs = size if len(label) <= 2 else int(size * 0.72)
        svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{size*2}" height="{size*2}" viewBox="0 0 {size*2} {size*2}">'
               f'<text x="{size}" y="{int(size*1.6)}" font-family="DejaVu Sans, sans-serif" font-size="{fs*2}" '
               f'font-weight="bold" fill="#ffffff" text-anchor="middle" stroke="#000000" stroke-width="1">{label}</text></svg>')
        subprocess.run(["rsvg-convert", "-w", str(size * 2), "-h", str(size * 2), "-o", tmp, "-"],
                       input=svg.encode(), check=True)
        return tmp
    if set_name == "switch":
        col, row = (int(v) for v in SWITCH_CELL[button].split(","))
        sheet = Image.open(os.path.join(pack_root, SWITCH_SHEET[6:])).convert("RGBA")
        cw, ch = sheet.width // 2, sheet.height // 2
        face = sheet.crop((col * cw, row * ch, (col + 1) * cw, (row + 1) * ch))
        face.resize((size, size), Image.LANCZOS).save(tmp)
        return tmp
    svg = os.path.join(svg_root, SVG[set_name][button])
    subprocess.run(["rsvg-convert", "-w", str(size), "-h", str(size), "-o", tmp, svg], check=True)
    return tmp


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--svg", required=True)
    ap.add_argument("--base", required=True, help="dumped font atlas PNG")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    svg_root = os.path.abspath(args.svg)
    pack_root = os.path.dirname(svg_root)
    base = Image.open(args.base).convert("RGBA")

    for set_name in ("xbox", "steam", "dualsense", "dualshock", "switch", "keyboard"):
        out_dir = os.path.join(args.out, set_name)
        os.makedirs(out_dir, exist_ok=True)
        sheet = base.copy()
        draw = ImageDraw.Draw(sheet)
        for (x, y, w, h, button) in CELLS:
            draw.rectangle([x, y, x + w - 1, y + h - 1], fill=(0, 0, 0, 0))
            inner = max(8, min(w, h) - 2)
            glyph = render_prompt_glyph(set_name, button, inner * 2, svg_root)
            if glyph is None:
                continue
            glyph = ImageOps.contain(glyph, (inner, inner), Image.LANCZOS)
            px = x + (w - glyph.width) // 2
            py = y + (h - glyph.height) // 2
            sheet.alpha_composite(glyph, (px, py))
        sheet.save(os.path.join(out_dir, SHEET_KEY))
        print(f"{set_name}: wrote {SHEET_KEY}")
    print("font sheets done")


if __name__ == "__main__":
    main()
