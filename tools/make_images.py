#!/usr/bin/env python3
"""Draws the two pictures of GDFWR with Pillow + numpy (no other files needed):

  resources/ring-button.png   256x256, transparent: the blue ring used as a button (panel + replay HUD)
  logo.png                    336x336: the mod logo Geode shows in the mod list

Run from the repo root:   python3 tools/make_images.py [--out DIR] [--logo-variant bars|ruler]
Everything is drawn 4x bigger and scaled down, so the edges are smooth. The ring is a GD blue orb (the reference: a dark outline, a bright cyan
band, a dark inner outline, and a white glow that fills the middle and fades out around the ring).
Fonts: Archivo Black (any other heavy font can be given with --font).
"""
import argparse
import os

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

SS = 4  # supersampling factor

# ---- the palette -------------------------------------------------------------------------------------------------------
NAVY = (4, 12, 30)            # outlines of the ring (near black, a little blue)
CYAN = (2, 242, 246)          # the band of the ring
GLOW = (255, 255, 240)        # the white glow
OUTLINE = (6, 20, 62)         # outline of the letters
EXTRUDE = (3, 12, 44)         # the "thickness" under the letters
# GD's blue background, sampled from a screenshot of the game
BG_TOP, BG_BOTTOM = (36, 112, 226), (22, 82, 198)
BG_STRIP = (43, 126, 255)
FLOOR_TOP, FLOOR_BOTTOM = (1, 101, 252), (0, 52, 168)
FLOOR_LINE = (190, 252, 255)
# the nine counter colours of the replay HUD, 1 ... 25+ (src/core/Metrics.cpp replayBucketColor, in the order of the HUD from the bottom up)
BUCKETS = [0xFF5C5C, 0xFFA85C, 0xFFF27A, 0xFFFFFF, 0x7CF08A, 0x7DB8FF, 0x7B7BFF, 0xB48CFF, 0xE3B5FF]


def rgb(c):
    return ((c >> 16) & 255, (c >> 8) & 255, c & 255)


def smoothstep(t):
    t = np.clip(t, 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def ring_image(size, outer_frac=0.84):
    """The blue ring as an RGBA image of size x size (already supersampled by the caller). outer_frac: radius of the dark outline / half the image."""
    s = size
    yy, xx = np.mgrid[0:s, 0:s].astype(np.float64)
    c = (s - 1) / 2.0
    d = np.hypot(xx - c, yy - c)
    ro = (s / 2.0) * outer_frac
    r = d / ro                                   # 1.0 = the outer edge of the dark outline
    img = np.zeros((s, s, 4), dtype=np.float64)
    img[..., 0:3] = GLOW
    # the glow: solid up to the outline, fading out beyond it
    img[..., 3] = 1.0 - smoothstep((r - 1.0) / 0.19)
    def paint(mask, col):
        for k in range(3):
            img[..., k] = np.where(mask, col[k], img[..., k])
        img[..., 3] = np.where(mask, 255.0 / 255.0, img[..., 3])
    paint((r <= 1.00) & (r > 0.86), NAVY)        # outer outline
    paint((r <= 0.86) & (r > 0.575), CYAN)       # the bright band
    paint((r <= 0.575) & (r > 0.42), NAVY)       # inner outline
    # (inside r <= 0.42 the glow shows through: the middle of the ring is white)
    out = (np.clip(img, 0, 255 if False else 1e9))
    out[..., 3] = np.clip(img[..., 3], 0.0, 1.0) * 255.0
    return Image.fromarray(np.clip(out, 0, 255).astype(np.uint8), "RGBA")


def ring_button(path):
    big = ring_image(256 * SS)
    im = big.resize((256, 256), Image.LANCZOS)
    im.save(path)
    return im


# ---- the logo ---------------------------------------------------------------------------------------------------------
def lerp(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def vertical_gradient(w, h, top, bottom):
    t = np.linspace(0.0, 1.0, h)[:, None, None]
    top = np.array(top, dtype=np.float64)[None, None, :]
    bottom = np.array(bottom, dtype=np.float64)[None, None, :]
    g = top + (bottom - top) * t
    return np.repeat(g, w, axis=1)


def find_font(explicit):
    cands = [explicit] if explicit else []
    cands += ["/usr/share/fonts/truetype/google-fonts/ofl/archivoblack/ArchivoBlack-Regular.ttf",
              "/usr/share/fonts/truetype/google-fonts/Poppins-Bold.ttf",
              "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"]
    for p in cands:
        if p and os.path.exists(p):
            return p
    raise SystemExit("no heavy font found: give one with --font")


def logo(path, font_path, variant):
    S = 336 * SS
    k = SS
    # background: GD blue with the lighter strips of its tiles
    bg = vertical_gradient(S, S, BG_TOP, BG_BOTTOM)
    img = Image.fromarray(bg.astype(np.uint8), "RGB").convert("RGBA")
    d = ImageDraw.Draw(img)
    for i in range(0, 4):                         # tile strips, 112 px apart, 12 px wide
        x = i * 112 * k
        d.rectangle([x - 6 * k, 0, x + 6 * k, S], fill=BG_STRIP + (150,))
        d.rectangle([0, x - 6 * k, S, x + 6 * k], fill=BG_STRIP + (150,))
    # a soft light behind the ring
    halo = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    hd = ImageDraw.Draw(halo)
    cx, cy = 168 * k, 104 * k
    hd.ellipse([cx - 150 * k, cy - 150 * k, cx + 150 * k, cy + 150 * k], fill=(120, 200, 255, 110))
    halo = halo.filter(ImageFilter.GaussianBlur(40 * k))
    img = Image.alpha_composite(img, halo)

    # the floor
    floor_y = 292 * k
    fl = vertical_gradient(S, S - floor_y, FLOOR_TOP, FLOOR_BOTTOM)
    img.paste(Image.fromarray(fl.astype(np.uint8), "RGB"), (0, floor_y))
    d = ImageDraw.Draw(img)
    d.rectangle([0, floor_y - 2 * k, S, floor_y + 3 * k], fill=FLOOR_LINE + (255,))

    # the ring
    ring_px = 190 * k
    ring = ring_image(ring_px, outer_frac=0.80)
    img.alpha_composite(ring, (cx - ring_px // 2, cy - ring_px // 2))

    # the letters "FW": white, dark outline, and a thick dark "extrusion" below them (the look of GD's big font)
    font = ImageFont.truetype(font_path, 124 * k)
    text = "FW"
    bbox = d.textbbox((0, 0), text, font=font)
    tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
    tx = (S - tw) // 2 - bbox[0]
    ty = 190 * k - bbox[1]
    stroke = 8 * k
    ext = 9 * k
    layer = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ld = ImageDraw.Draw(layer)
    for dy in range(ext, 0, -k // 2):             # the extrusion: the outline repeated downwards
        ld.text((tx, ty + dy), text, font=font, fill=EXTRUDE + (255,), stroke_width=stroke, stroke_fill=EXTRUDE + (255,))
    ld.text((tx, ty), text, font=font, fill=(255, 255, 255, 255), stroke_width=stroke, stroke_fill=OUTLINE + (255,))
    img = Image.alpha_composite(img, layer)
    # a light bottom on the letters: white at the top, pale blue at the bottom
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).text((tx, ty), text, font=font, fill=255)
    shade = vertical_gradient(S, S, (255, 255, 255), (255, 255, 255))
    ys = np.linspace(0, 1, S)[:, None, None]
    top_y, bot_y = ty + bbox[1], ty + bbox[3]
    t = np.clip((np.arange(S)[:, None, None] - top_y) / max(1, (bot_y - top_y)), 0, 1)
    shade = np.array((255, 255, 255))[None, None, :] * (1 - t * 0.0) + (np.array((176, 226, 255)) - np.array((255, 255, 255)))[None, None, :] * (t ** 1.6)
    shade = np.repeat(shade, S, axis=1)
    shade_img = Image.fromarray(np.clip(shade, 0, 255).astype(np.uint8), "RGB").convert("RGBA")
    shade_img.putalpha(mask)
    img = Image.alpha_composite(img, shade_img)

    d = ImageDraw.Draw(img)
    if variant == "bars":
        # nine bars, one per label 1 ... 25+, in the colours of the replay HUD, growing like a window histogram
        n = 9
        bw, gap = 22 * k, 6 * k
        total = n * bw + (n - 1) * gap
        x0 = (S - total) // 2
        base = 328 * k
        for i, col in enumerate(BUCKETS):
            h = int((8 + i * 2.5) * k)
            x = x0 + i * (bw + gap)
            d.rounded_rectangle([x - 2 * k, base - h - 2 * k, x + bw + 2 * k, base + 2 * k], radius=4 * k, fill=OUTLINE + (255,))
            d.rounded_rectangle([x, base - h, x + bw, base], radius=3 * k, fill=rgb(col) + (255,))
    else:
        # a ruler of frame ticks with a bracket around the window
        n = 25
        x0, step = 20 * k, 12.2 * k
        for i in range(n):
            x = int(x0 + i * step)
            inside = 9 <= i <= 15
            h = (22 if i % 4 == 0 else 14) * k
            col = CYAN if inside else (150, 210, 255)
            d.rectangle([x - 2 * k, floor_y + 8 * k, x + 2 * k, floor_y + 8 * k + h], fill=col + (255,))
        bl, br = int(x0 + 8.4 * step), int(x0 + 15.6 * step)
        for bx, sgn in ((bl, 1), (br, -1)):
            d.rectangle([bx - 3 * k, floor_y + 4 * k, bx + 3 * k, floor_y + 40 * k], fill=(255, 255, 255, 255))
            for y0 in (floor_y + 4 * k, floor_y + 35 * k):   # the two arms of the bracket, pointing inwards
                xa, xb = sorted((bx - 3 * k, bx + sgn * 12 * k))
                d.rectangle([xa, y0, xb, y0 + 5 * k], fill=(255, 255, 255, 255))

    im = img.convert("RGB").resize((336, 336), Image.LANCZOS)
    im.save(path)
    return im


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=".", help="the repo root (logo.png goes here, ring-button.png into resources/)")
    ap.add_argument("--font", default=None)
    ap.add_argument("--logo-variant", default="bars", choices=["bars", "ruler"])
    a = ap.parse_args()
    os.makedirs(os.path.join(a.out, "resources"), exist_ok=True)
    ring_button(os.path.join(a.out, "resources", "ring-button.png"))
    logo(os.path.join(a.out, "logo.png"), find_font(a.font), a.logo_variant)
    print("wrote", os.path.join(a.out, "logo.png"), "and", os.path.join(a.out, "resources", "ring-button.png"))


if __name__ == "__main__":
    main()
