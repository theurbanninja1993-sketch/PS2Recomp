#!/usr/bin/env python3
"""Build button-prompt replacement packs for the VCS port.

The runtime replaces textures by PCSX2 key (tex0Hash-clutHash-bits), and the
prompt sets live in prompts/<set>/ next to the executable (xbox, steam,
keyboard, playstation, switch), loaded after the mods so they always win.

Workflow (once the game's prompt-texture keys are known; PS2X_TEXREPL_DUMP
captures them from gameplay/menu screens):

  1. Identify which dumped textures are the button glyphs and for each one
     decide the layout: either a single glyph covering the texture, or an
     atlas with cells (classify the PS glyphs by shape: cross/triangle/
     circle/square/d-pad/start/select/shoulders).
  2. Write packs.json describing every replacement:
       {
         "<tex0Hash>-<clutHash>-<bits>.png": {
           "cells": [ {"x":0,"y":0,"w":32,"h":32,"button":"cross"} ]
         }
       }
     for single-glyph textures give one full-canvas cell.
  3. Run:  make_prompt_packs.py --svg <AL2009man prompt-pack dir> \\
            [--keyboard-font <ttf>] [--out <prompts dir>]

Buttons are mapped per set (xbox/steam/playstation/switch use the AL2009man
SVGs; keyboard renders keycap text with the label in the mapping, defaulting
to the port's default bindings for the action the shape usually represents).

The SVG glyph names in the AL2009man pack look like ps-Cross.svg,
ps-Triangle.svg, xbox-P1.svg (P1..P4 = A/B/X/Y by position), etc.
"""

import argparse
import json
import os
import subprocess
import sys

# PS2 shape -> per-set glyph (relative to the SVG pack root), square target
# size; keyboard labels. Verified against the checked-out pack (2026-10-13):
# - Xbox face buttons only exist as positional P1..P4 (diamond order
#   A=bottom, B=right, X=left, Y=top - to be confirmed visually); no bumpers
#   or D-pad in the prompt pack.
# - PlayStation face buttons are the generic ps-Cross/Circle/Square/Triangle
#   (COLOR variants available); ps4/ps5 buttons for start/select.
# - Steam and Switch have NO single-glyph face SVGs: Steam falls back to the
#   Xbox glyphs (the Deck's A/B/X/Y are Xbox-positioned); Switch uses the
#   Pro Controller FaceButton sheet (2x2 diamond: bottom=A=confirm).
SVG_NAMES = {
    "xbox": {
        "cross": "Xbox/xbox-P1.svg",      # A (bottom)
        "circle": "Xbox/xbox-P2.svg",     # B (right)
        "square": "Xbox/xbox-P3.svg",     # X (left)
        "triangle": "Xbox/xbox-P4.svg",   # Y (top)
        "start": "Xbox/xbox-Menu.svg",
        "select": "Xbox/xbox-View.svg",
        # No Xbox D-pad glyphs in the pack: the generic arrows are universal.
        "dpad_up": "PlayStation/ps-D-PAD Up.svg",
        "dpad_down": "PlayStation/ps-D-PAD Down.svg",
        "dpad_left": "PlayStation/ps-D-PAD Left.svg",
        "dpad_right": "PlayStation/ps-D-PAD Right.svg",
    },
    "dualsense": {
        "cross": "PlayStation/ps-Cross.svg",
        "circle": "PlayStation/ps-Circle.svg",
        "square": "PlayStation/ps-Square.svg",
        "triangle": "PlayStation/ps-Triangle.svg",
        "dpad": "PlayStation/ps-D-PAD.svg",
        "dpad_up": "PlayStation/ps-D-PAD Up.svg",
        "dpad_down": "PlayStation/ps-D-PAD Down.svg",
        "dpad_left": "PlayStation/ps-D-PAD Left.svg",
        "dpad_right": "PlayStation/ps-D-PAD Right.svg",
        "start": "PlayStation/ps5-Option.svg",
        "select": "PlayStation/ps5-Create.svg",
        "l1": "PlayStation/ps5-Left Back.svg",
        "r1": "PlayStation/ps5-Right Back.svg",
    },
    "dualshock": {
        "cross": "PlayStation/ps-Cross.svg",
        "circle": "PlayStation/ps-Circle.svg",
        "square": "PlayStation/ps-Square.svg",
        "triangle": "PlayStation/ps-Triangle.svg",
        "dpad": "PlayStation/ps-D-PAD.svg",
        "dpad_up": "PlayStation/ps-D-PAD Up.svg",
        "dpad_down": "PlayStation/ps-D-PAD Down.svg",
        "dpad_left": "PlayStation/ps-D-PAD Left.svg",
        "dpad_right": "PlayStation/ps-D-PAD Right.svg",
        "start": "PlayStation/ps4-Option.svg",
        "select": "PlayStation/ps4-Share.svg",
        "l1": "PlayStation/ps5-Left Back.svg",  # no DS4 shoulder glyphs in the pack
        "r1": "PlayStation/ps5-Right Back.svg",
    },
    "switch": {
        # Pro Controller FaceButton sheet: 2x2 diamond, A at the bottom cell.
        "cross": "sheet:Controller Asset Pack/Nintendo Switch Controller Images/Switch Pro Controller/"
                 "Default Theme/Theme Assets/Active Presses/NSwitchPro_FaceButton.png#0,1",
        "circle": "sheet:Controller Asset Pack/Nintendo Switch Controller Images/Switch Pro Controller/"
                  "Default Theme/Theme Assets/Active Presses/NSwitchPro_FaceButton.png#1,1",
        "square": "sheet:Controller Asset Pack/Nintendo Switch Controller Images/Switch Pro Controller/"
                  "Default Theme/Theme Assets/Active Presses/NSwitchPro_FaceButton.png#0,0",
        "triangle": "sheet:Controller Asset Pack/Nintendo Switch Controller Images/Switch Pro Controller/"
                    "Default Theme/Theme Assets/Active Presses/NSwitchPro_FaceButton.png#1,0",
        "dpad": "Nintendo/nintendoswitch-JoyCon D-PAD.svg",
        "dpad_up": "Nintendo/nintendoswitch-JoyCon D-PAD Up.svg",
        "dpad_down": "Nintendo/nintendoswitch-JoyCon D-PAD Down.svg",
        "dpad_left": "Nintendo/nintendoswitch-JoyCon D-PAD Left.svg",
        "dpad_right": "Nintendo/nintendoswitch-JoyCon D-PAD Right.svg",
        "start": "Nintendo/nintendo-Plus.svg",
        "select": "Nintendo/nintendo-Minus.svg",
    },
    "steam": {
        # Steam Deck A/B/X/Y are Xbox-positioned; the pack ships no individual
        # Steam face glyphs, so the Xbox set is used (swap art later).
        "cross": "Xbox/xbox-P1.svg",
        "circle": "Xbox/xbox-P2.svg",
        "square": "Xbox/xbox-P3.svg",
        "triangle": "Xbox/xbox-P4.svg",
        "dpad_up": "PlayStation/ps-D-PAD Up.svg",
        "dpad_down": "PlayStation/ps-D-PAD Down.svg",
        "dpad_left": "PlayStation/ps-D-PAD Left.svg",
        "dpad_right": "PlayStation/ps-D-PAD Right.svg",
    },
    "keyboard": {  # keycap text labels; adjust to the port's default bindings
        "cross": "E",
        "circle": "BACKSPACE",
        "square": "RMB",
        "triangle": "SPACE",
        "dpad": "WASD",
        "start": "ESC",
        "select": "TAB",
    },
}

DEFAULT_KEYCAP = "?"  # glyphs without an entry (e.g. l1/r1) use this


def rasterize(svg_path, out_png, size):
    subprocess.run(["rsvg-convert", "-w", str(size), "-h", str(size), "-o", out_png, svg_path],
                   check=True)


def make_keycap(label, out_png, size, font=None):
    # Simple SVG keycap; rsvg renders it. Font defaults to a system sans.
    svg = f"""<svg xmlns="http://www.w3.org/2000/svg" width="{size}" height="{size}" viewBox="0 0 32 32">
<rect x="2" y="2" width="28" height="28" rx="6" fill="#20262e" stroke="#8a94a6" stroke-width="2"/>
<text x="16" y="21" font-family="{font or 'DejaVu Sans, sans-serif'}" font-size="14"
      font-weight="bold" fill="#e8ecf2" text-anchor="middle">{label}</text>
</svg>"""
    subprocess.run(["rsvg-convert", "-w", str(size), "-h", str(size), "-o", out_png, "-"],
                   input=svg.encode(), check=True)


def compose(canvas_w, canvas_h, cells, glyphs, out_png):
    from PIL import Image, ImageOps
    canvas = Image.new("RGBA", (canvas_w, canvas_h), (0, 0, 0, 0))
    previews = {}   # button -> preview PNG path
    for cell in cells:
        key = cell["button"]
        src = Image.open(glyphs[key])
        src = ImageOps.contain(src, (cell["w"], cell["h"]))
        x = cell["x"] + (cell["w"] - src.width) // 2
        y = cell["y"] + (cell["h"] - src.height) // 2
        canvas.alpha_composite(src, (x, y))
        # per-button preview tile for the settings menu
        tile = Image.new("RGBA", (cell["w"], cell["h"]), (0, 0, 0, 0))
        tile.alpha_composite(src, (x - cell["x"], y - cell["y"]))
        previews[key] = tile
    canvas.save(out_png)
    return previews


def compose_controller(out_png, art_path, mode, w, h):
    """Fit the device's controller art into the game's diagram texture
    (transparent background, centered)."""
    from PIL import Image, ImageOps
    art = Image.open(art_path).convert("RGBA")
    if mode == "cover":
        art = ImageOps.fit(art, (w, h), Image.LANCZOS)
    else:
        art = ImageOps.contain(art, (int(w * 0.9), int(h * 0.9)), Image.LANCZOS)
    canvas = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    canvas.alpha_composite(art, ((w - art.width) // 2, (h - art.height) // 2))
    canvas.save(out_png)
    return canvas


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--svg", required=True, help="AL2009man Gamepad-Prompt-Asset-Pack directory")
    ap.add_argument("--packs", default="packs.json", help="manifest (see docstring)")
    ap.add_argument("--out", default=".", help="output prompts directory")
    ap.add_argument("--keyboard-font", default=None)
    args = ap.parse_args()

    manifest = json.load(open(args.packs))
    for set_name, textures in manifest.items():
        mapping = SVG_NAMES.get(set_name, {})
        outdir = os.path.join(args.out, set_name)
        os.makedirs(outdir, exist_ok=True)
        glyph_cache = {}
        # The AL2009man pack root: contains Gamepad-Prompt-Asset-Pack/ (the
        # SVGs) and Controller Asset Pack/ (the controller art), so --svg
        # points at .../Gamepad-Prompt-Asset-Pack and art paths are relative
        # to its parent.
        pack_root = os.path.dirname(os.path.abspath(args.svg))
        manifest = []  # button=preview-file lines for the settings menu
        previews = {}  # button -> PIL tile
        for file_name, spec in textures.items():
            if file_name == "_controller":
                continue  # handled after the glyph textures
            cells = spec["cells"]
            canvas_w = max(c["x"] + c["w"] for c in cells)
            canvas_h = max(c["y"] + c["h"] for c in cells)
            for cell in cells:
                btn = cell["button"]
                if btn in glyph_cache:
                    continue
                size = cell["w"]  # glyphs are square
                if set_name == "keyboard":
                    label = mapping.get(btn, DEFAULT_KEYCAP)
                    tmp = os.path.join(outdir, f"_glyph_{btn}.png")
                    make_keycap(label, tmp, size, args.keyboard_font)
                elif str(mapping.get(btn, "")).startswith("sheet:"):
                    # A cell of a PNG sheet: sheet:<path>#col,row
                    sheet_path, _, cellPos = mapping[btn][6:].partition("#")
                    col, row = (int(v) for v in cellPos.split(","))
                    from PIL import Image
                    sheet = Image.open(os.path.join(pack_root, sheet_path)).convert("RGBA")
                    cw, ch = sheet.width // 2, sheet.height // 2
                    face = sheet.crop((col * cw, row * ch, (col + 1) * cw, (row + 1) * ch))
                    tmp = os.path.join(outdir, f"_glyph_{btn}.png")
                    face.resize((size, size), Image.LANCZOS).save(tmp)
                else:
                    svg = os.path.join(args.svg, mapping.get(btn, ""))
                    if not os.path.exists(svg):
                        print(f"!! {set_name}: no SVG for '{btn}' (wanted {svg}); skipping", file=sys.stderr)
                        continue
                    tmp = os.path.join(outdir, f"_glyph_{btn}.png")
                    rasterize(svg, tmp, size)
                glyph_cache[btn] = tmp
            missing = [c["button"] for c in cells if c["button"] not in glyph_cache]
            if missing:
                print(f"!! {set_name}/{file_name}: missing glyphs {missing}; skipping", file=sys.stderr)
                continue
            per_button = compose(canvas_w, canvas_h, cells, glyph_cache, os.path.join(outdir, file_name))
            for btn, tile in per_button.items():
                previews[btn] = tile  # last cell placement wins for multi-file buttons
        # Controller diagram art for this set (optional "_controller" entry).
        if "_controller" in textures:
            cspec = textures["_controller"]
            target = cspec.get("file", "")
            art = cspec.get("art", "")
            if target and art and os.path.exists(os.path.join(pack_root, art)):
                from PIL import Image
                gw, gh = Image.open(os.path.join(pack_root, target)).size if os.path.exists(
                    os.path.join(pack_root, target)) else (512, 256)
                compose_controller(os.path.join(outdir, target), os.path.join(pack_root, art),
                                   cspec.get("mode", "fit"), gw, gh)
                manifest.append(f"controller={target}")
        # Per-button preview tiles + manifest for the settings menu.
        for btn, tile in previews.items():
            pv = f"preview_{btn}.png"
            tile.save(os.path.join(outdir, pv))
            manifest.append(f"{btn}={pv}")
        with open(os.path.join(outdir, "manifest.txt"), "w") as mf:
            mf.write("\n".join(manifest) + ("\n" if manifest else ""))
        for btn, tmp in glyph_cache.items():
            os.unlink(tmp)
    print("done: packs written under", args.out)


if __name__ == "__main__":
    main()