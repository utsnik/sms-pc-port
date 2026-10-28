#!/usr/bin/env python3
"""Draw the Wii U Menu artwork for the SMS port (icon, TV and GamePad boot screens).

Everything is drawn from primitives - no Nintendo artwork. Usage: make_art.py OUTDIR
"""
import math
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

SS = 4  # supersample factor
BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"

SKY_TOP = (18, 92, 196)
SKY_BOTTOM = (120, 206, 240)
SEA_TOP = (22, 160, 190)
SEA_BOTTOM = (8, 84, 140)
SUN = (255, 214, 40)
SUN_CORE = (255, 240, 150)
RAY = (255, 230, 120)
SAND = (244, 214, 140)
PALM = (26, 110, 60)
TRUNK = (128, 84, 44)


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def vgradient(draw, box, top, bottom):
    x0, y0, x1, y1 = box
    h = max(1, y1 - y0)
    for y in range(y0, y1):
        draw.line([(x0, y), (x1, y)], fill=lerp(top, bottom, (y - y0) / h))


def scene(w, h, sun_c, sun_r, horizon, island=True):
    """Sky, sun with rays, sea with wave highlights, island with a palm."""
    img = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(img)
    vgradient(d, (0, 0, w, horizon), SKY_TOP, SKY_BOTTOM)

    # soft rays: translucent wedges around the sun, blurred
    rays = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    rd = ImageDraw.Draw(rays)
    cx, cy = sun_c
    reach = max(w, h) * 1.5
    n = 16
    for i in range(n):
        a0 = (i / n) * 2 * math.pi
        a1 = a0 + math.pi / n * 0.55
        rd.polygon([(cx, cy),
                    (cx + reach * math.cos(a0), cy + reach * math.sin(a0)),
                    (cx + reach * math.cos(a1), cy + reach * math.sin(a1))],
                   fill=RAY + (60,))
    rays = rays.filter(ImageFilter.GaussianBlur(sun_r * 0.08))
    img.paste(rays, (0, 0), rays)

    # glow + sun
    glow = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    gr = sun_r * 1.6
    gd.ellipse([cx - gr, cy - gr, cx + gr, cy + gr], fill=(255, 240, 160, 110))
    glow = glow.filter(ImageFilter.GaussianBlur(sun_r * 0.35))
    img.paste(glow, (0, 0), glow)
    d = ImageDraw.Draw(img)
    d.ellipse([cx - sun_r, cy - sun_r, cx + sun_r, cy + sun_r], fill=SUN)
    cr = sun_r * 0.72
    d.ellipse([cx - cr - sun_r * 0.12, cy - cr - sun_r * 0.12,
               cx + cr - sun_r * 0.12, cy + cr - sun_r * 0.12], fill=lerp(SUN, SUN_CORE, 0.6))

    # sea
    vgradient(d, (0, horizon, w, h), SEA_TOP, SEA_BOTTOM)
    rows = 7
    for r in range(rows):
        t = (r + 1) / (rows + 1)
        y = horizon + (h - horizon) * t
        amp = (h - horizon) * 0.018 * (0.5 + t)
        seg = w / (6 + r * 2)
        lw = max(1, int((h - horizon) * 0.012 * (0.6 + t)))
        x = -(r * seg * 0.37) % seg - seg
        while x < w:
            d.arc([x, y - amp, x + seg * 0.6, y + amp], 200, 340,
                   fill=(200, 240, 250), width=lw)
            x += seg

    if island:
        iw = w * 0.30
        ix = w * 0.80
        ih = (h - horizon) * 0.22
        d.ellipse([ix - iw / 2, horizon - ih * 0.55, ix + iw / 2, horizon + ih * 0.55], fill=SAND)
        # trunk: a leaning curve of circles
        base = (ix - iw * 0.08, horizon - ih * 0.3)
        top = (ix + iw * 0.06, horizon - h * 0.30)
        tw = w * 0.012
        for i in range(60):
            t = i / 59
            px = base[0] + (top[0] - base[0]) * t + math.sin(t * math.pi) * iw * 0.10
            py = base[1] + (top[1] - base[1]) * t
            rr = tw * (1.15 - 0.4 * t)
            d.ellipse([px - rr, py - rr, px + rr, py + rr], fill=TRUNK)
        # fronds: drooping leaf polygons
        fl = h * 0.17
        for ang in (-170, -135, -100, -60, -25, 10, 200):
            a = math.radians(ang)
            pts = []
            for i in range(21):
                t = i / 20
                px = top[0] + math.cos(a) * fl * t
                py = top[1] + math.sin(a) * fl * t + (fl * 0.45) * t * t
                pts.append((px, py))
            wdt = fl * 0.11
            upper = [(x, y - wdt * math.sin(math.pi * i / 20)) for i, (x, y) in enumerate(pts)]
            lower = [(x, y + wdt * math.sin(math.pi * i / 20)) for i, (x, y) in enumerate(pts)]
            d.polygon(upper + lower[::-1], fill=PALM)
        d.ellipse([top[0] - tw * 1.6, top[1] - tw * 1.6, top[0] + tw * 1.6, top[1] + tw * 1.6], fill=PALM)
    return img


def text(img, lines, x, y, sizes, align="left", shadow=True):
    d = ImageDraw.Draw(img)
    cy = y
    for line, size in zip(lines, sizes):
        f = ImageFont.truetype(BOLD, size)
        bbox = d.textbbox((0, 0), line, font=f, stroke_width=max(2, size // 12))
        tw = bbox[2] - bbox[0]
        tx = x - tw / 2 if align == "center" else x
        sw = max(2, size // 12)
        if shadow:
            off = max(2, size // 14)
            d.text((tx + off, cy + off), line, font=f, fill=(10, 40, 90),
                   stroke_width=sw, stroke_fill=(10, 40, 90))
        d.text((tx, cy), line, font=f, fill=(255, 255, 255), stroke_width=sw, stroke_fill=(14, 60, 130))
        cy += (bbox[3] - bbox[1]) * 1.12
    return cy


def boot(w, h):
    W, H = w * SS, h * SS
    horizon = int(H * 0.66)
    img = scene(W, H, (int(W * 0.78), int(H * 0.27)), int(H * 0.16), horizon)
    s = H / 720
    bottom = text(img, ["Super Mario", "Sunshine"], int(W * 0.06), int(H * 0.13),
                  [int(104 * s), int(136 * s)])
    sub = ImageFont.truetype(BOLD, int(40 * s))
    d = ImageDraw.Draw(img)
    d.text((int(W * 0.068), int(bottom + 18 * s)), "Wii U port", font=sub, fill=(255, 250, 220),
           stroke_width=int(4 * s), stroke_fill=(14, 60, 130))
    return img.resize((w, h), Image.LANCZOS)


def icon():
    W = H = 128 * SS
    horizon = int(H * 0.70)
    img = scene(W, H, (int(W * 0.72), int(H * 0.30)), int(H * 0.20), horizon, island=False)
    text(img, ["Mario", "Sunshine"], W // 2, int(H * 0.40), [int(H * 0.17), int(H * 0.17)],
         align="center")
    return img.resize((128, 128), Image.LANCZOS)


def main():
    out = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    out.mkdir(parents=True, exist_ok=True)
    boot(1280, 720).save(out / "tv.png")
    boot(854, 480).save(out / "drc.png")
    icon().save(out / "icon.png")


if __name__ == "__main__":
    main()
