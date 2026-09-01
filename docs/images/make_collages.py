#!/usr/bin/env python3
"""Build the README collages from the raw screenshot crops in this folder.

    python docs/images/make_collages.py

Outputs (all in docs/images): hero.jpg, assembly.png, support-matching.png, materials.png,
print-quality.png, compare-slices.png. Re-run after replacing any source crop.
"""
import math
import os
import textwrap

from PIL import Image, ImageDraw, ImageFont

D = os.path.dirname(os.path.abspath(__file__))
BG = (24, 24, 24)
BORDER = (70, 70, 70)
TXT = (222, 222, 222)
PAD, GAP, CAP_LINE = 20, 20, 24


def font(size=18, bold=False):
    for name in (["segoeuib.ttf", "arialbd.ttf"] if bold else ["segoeui.ttf", "arial.ttf"]):
        p = os.path.join(os.environ.get("WINDIR", "C:/Windows"), "Fonts", name)
        if os.path.exists(p):
            return ImageFont.truetype(p, size)
    return ImageFont.load_default()


F, FB = font(18), font(20, True)


def load(name):
    return Image.open(os.path.join(D, name)).convert("RGB")


def fit_h(im, h):
    return im.resize((round(im.width * h / im.height), h), Image.LANCZOS)


def scale(im, s):
    return im.resize((round(im.width * s), round(im.height * s)), Image.LANCZOS)


def cover(im, w, h):
    s = max(w / im.width, h / im.height)
    im = im.resize((math.ceil(im.width * s), math.ceil(im.height * s)), Image.LANCZOS)
    x, y = (im.width - w) // 2, (im.height - h) // 2
    return im.crop((x, y, x + w, y + h))


def wrap(text, width_px):
    d = ImageDraw.Draw(Image.new("RGB", (1, 1)))
    lines, line = [], ""
    for word in text.split():
        cand = (line + " " + word).strip()
        if d.textlength(cand, font=F) <= width_px or not line:
            line = cand
        else:
            lines.append(line)
            line = word
    if line:
        lines.append(line)
    return lines


def panel(im, caption=None, min_w=0):
    """Screenshot with a 1px border and an optional wrapped caption underneath."""
    w = max(im.width + 2, min_w)
    lines = wrap(caption, w) if caption else []
    h = im.height + 2 + (8 + CAP_LINE * len(lines) if lines else 0)
    out = Image.new("RGB", (w, h), BG)
    x0 = (w - im.width - 2) // 2
    d = ImageDraw.Draw(out)
    d.rectangle((x0, 0, x0 + im.width + 1, im.height + 1), outline=BORDER)
    out.paste(im, (x0 + 1, 1))
    y = im.height + 2 + 6
    for ln in lines:
        d.text(((w - d.textlength(ln, font=F)) / 2, y), ln, font=F, fill=TXT)
        y += CAP_LINE
    return out


def grid(rows, valign="top", halign="center"):
    """rows: list of lists of images. Each row is laid out horizontally; rows stack vertically."""
    row_w = [sum(p.width for p in r) + GAP * (len(r) - 1) for r in rows]
    row_h = [max(p.height for p in r) for r in rows]
    W = max(row_w) + 2 * PAD
    H = sum(row_h) + GAP * (len(rows) - 1) + 2 * PAD
    out = Image.new("RGB", (W, H), BG)
    y = PAD
    for r, rw, rh in zip(rows, row_w, row_h):
        x = PAD + ((W - 2 * PAD - rw) // 2 if halign == "center" else 0)
        for p in r:
            dy = (rh - p.height) // 2 if valign == "center" else 0
            out.paste(p, (x, y + dy))
            x += p.width + GAP
        y += rh + GAP
    return out


def label(im, text):
    """Semi-transparent label in the top-left corner (hero tiles)."""
    im = im.convert("RGBA")
    ov = Image.new("RGBA", im.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(ov)
    tw = d.textlength(text, font=FB)
    d.rectangle((12, 12, 12 + tw + 20, 12 + 34), fill=(0, 0, 0, 160))
    d.text((22, 16), text, font=FB, fill=(255, 255, 255, 255))
    return Image.alpha_composite(im, ov).convert("RGB")


def save(im, name, **kw):
    path = os.path.join(D, name)
    im.save(path, optimize=True, **kw)
    print(f"{name}: {im.width}x{im.height}, {os.path.getsize(path) // 1024} KB")


def main():
    # --- hero: 2x2 teaser tiles -------------------------------------------------------------
    TW, TH = 680, 425
    stream = cover(load("menu-stream.jpg"), TW, TH)
    support = cover(load("supportmatch-normal.png"), TW, TH)
    compare = cover(load("compare-ui.png"), TW, TH)
    asm_tile = Image.new("RGB", (TW, TH), BG)
    top = 58  # leave room for the corner label
    a1, a2 = fit_h(load("assembly-menuupdate.png"), TH - top - 12), scale(load("assembly-newtypes.png"), 0.95)
    x = (TW - (a1.width + 16 + a2.width)) // 2
    asm_tile.paste(a1, (x, top))
    asm_tile.paste(a2, (x + a1.width + 16, top + (a1.height - a2.height) // 2))
    tiles = [label(stream, "Stream tab"), label(support, "Support Filament Matching"),
             label(compare, "Compare Slices"), label(asm_tile, "Assemble tool: Auto-fit")]
    save(grid([tiles[:2], tiles[2:]]), "hero.jpg", quality=88)

    # --- assembly ---------------------------------------------------------------------------
    save(grid([[panel(load("assembly-newtypes.png"), "Four assembly modes: face, point, triangle, curve"),
                panel(load("assembly-menuupdate.png"),
                      "Curve-and-curve mate: Auto-fit, live Rotate / Offset sliders, then Merge parts")]]),
         "assembly.png")

    # --- support matching -------------------------------------------------------------------
    tree = load("supportmatch-tree.png")
    save(grid([[panel(fit_h(load("supportmatch-normal.png"), tree.height), "Normal supports"),
                panel(tree, "Tree supports")],
               [panel(load("supportmatch-option.png"), "Print settings → Support → Support Filament Matching")]]),
         "support-matching.png")

    # --- materials --------------------------------------------------------------------------
    save(grid([[panel(load("filament-applyall.png"), "Apply All: set every filament slot at once"),
                panel(load("features-outerwallfilament.png"), "Filament for Features → Outer wall")],
               [panel(load("options-spoolman.png"), "Preferences → Ultra → Spool Manager")]]),
         "materials.png")

    # --- print quality ----------------------------------------------------------------------
    zo = load("features-zoverridexy.png")
    zo = zo.crop((0, 10, zo.width, zo.height))  # drop the half-visible row above the option
    save(grid([[panel(load("features-offsetwall.png"))], [panel(zo)]]), "print-quality.png")

    # --- compare slices ---------------------------------------------------------------------
    save(grid([[panel(load("compare-menu.png"), "Preview toolbar"),
                panel(load("compare-ui.png"),
                      "Compare Slices: time / filament deltas, changed settings and a per-layer toolpath overlay")]],
               valign="center"),
         "compare-slices.png")


if __name__ == "__main__":
    main()
