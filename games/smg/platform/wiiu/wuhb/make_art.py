#!/usr/bin/env python3
"""Draw the Wii U Menu artwork for the Galaxy port (icon, TV and GamePad boot screens).

Everything is drawn from primitives - no Nintendo artwork. Usage: make_art.py OUTDIR
"""
import math
import random
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

SS = 4  # supersample factor
BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"

SPACE_TOP = (8, 10, 40)
SPACE_BOTTOM = (46, 20, 92)
NEBULA = (120, 70, 200)
STAR = (255, 214, 40)
STAR_CORE = (255, 244, 170)
PLANET_LIT = (120, 210, 110)
PLANET_DARK = (24, 92, 70)
SMALL_LIT = (250, 170, 90)
SMALL_DARK = (150, 60, 50)
RING = (200, 190, 255)


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def vgradient(draw, box, top, bottom):
    x0, y0, x1, y1 = box
    h = max(1, y1 - y0)
    for y in range(y0, y1):
        draw.line([(x0, y), (x1, y)], fill=lerp(top, bottom, (y - y0) / h))


def glow(img, c, r, color, alpha, blur):
    g = Image.new("RGBA", img.size, (0, 0, 0, 0))
    ImageDraw.Draw(g).ellipse([c[0] - r, c[1] - r, c[0] + r, c[1] + r], fill=color + (alpha,))
    g = g.filter(ImageFilter.GaussianBlur(blur))
    img.paste(g, (0, 0), g)


def sphere(img, c, r, lit, dark):
    """A shaded ball: concentric discs from the dark rim to a lit spot up-left."""
    d = ImageDraw.Draw(img)
    steps = 48
    for i in range(steps):
        t = i / (steps - 1)
        rr = r * (1 - t * 0.82)
        ox = -r * 0.32 * t
        oy = -r * 0.32 * t
        d.ellipse([c[0] + ox - rr, c[1] + oy - rr, c[0] + ox + rr, c[1] + oy + rr],
                  fill=lerp(dark, lit, t ** 0.7))


def star_points(cx, cy, outer, inner, rot=-math.pi / 2):
    pts = []
    for i in range(10):
        a = rot + i * math.pi / 5
        rr = outer if i % 2 == 0 else inner
        pts.append((cx + rr * math.cos(a), cy + rr * math.sin(a)))
    return pts


def scene(w, h, star_c, star_r, planet=True):
    """Night sky with stars and a nebula, a five-pointed star, a ringed planet."""
    img = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(img)
    vgradient(d, (0, 0, w, h), SPACE_TOP, SPACE_BOTTOM)

    # nebula wash
    neb = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    nd = ImageDraw.Draw(neb)
    nd.ellipse([w * 0.15, h * 0.35, w * 0.85, h * 1.1], fill=NEBULA + (70,))
    nd.ellipse([w * 0.55, -h * 0.2, w * 1.2, h * 0.5], fill=(60, 120, 220, 60))
    neb = neb.filter(ImageFilter.GaussianBlur(min(w, h) * 0.12))
    img.paste(neb, (0, 0), neb)

    # background stars (seeded, so the art is reproducible)
    rng = random.Random(1985)
    d = ImageDraw.Draw(img)
    for _ in range(int(w * h / 9000 / SS)):
        x, y = rng.random() * w, rng.random() * h
        rr = (0.6 + rng.random() * 1.6) * SS * (w / (1280 * SS)) ** 0.5
        b = 170 + int(rng.random() * 85)
        d.ellipse([x - rr, y - rr, x + rr, y + rr], fill=(b, b, min(255, b + 20)))

    # the big star with a soft glow and a twinkle cross
    cx, cy = star_c
    glow(img, star_c, star_r * 2.2, (255, 230, 120), 90, star_r * 0.6)
    d = ImageDraw.Draw(img)
    for a in (0, math.pi / 2):
        ln = star_r * 1.5
        d.line([(cx - ln * math.cos(a), cy - ln * math.sin(a)),
                (cx + ln * math.cos(a), cy + ln * math.sin(a))],
               fill=(255, 245, 200), width=max(1, int(star_r * 0.03)))
    d.polygon(star_points(cx, cy, star_r, star_r * 0.45), fill=STAR)
    d.polygon(star_points(cx - star_r * 0.06, cy - star_r * 0.08, star_r * 0.6, star_r * 0.27),
              fill=lerp(STAR, STAR_CORE, 0.6))

    if planet:
        # big planet low on the right, with a tilted ring drawn in two halves
        pc = (w * 0.80, h * 0.80)
        pr = h * 0.26
        ring = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        rd = ImageDraw.Draw(ring)
        box = [pc[0] - pr * 1.75, pc[1] - pr * 0.42, pc[0] + pr * 1.75, pc[1] + pr * 0.42]
        rd.ellipse(box, outline=RING + (200,), width=max(2, int(pr * 0.06)))
        ring = ring.rotate(-14, center=pc, resample=Image.BICUBIC)
        back = ring.crop((0, 0, w, int(pc[1]))).copy()
        img.paste(back, (0, 0), back)
        glow(img, pc, pr * 1.05, (120, 230, 160), 70, pr * 0.12)
        sphere(img, pc, pr, PLANET_LIT, PLANET_DARK)
        front = ring.crop((0, int(pc[1]), w, h)).copy()
        img.paste(front, (0, int(pc[1])), front)
        # a small moon
        sphere(img, (w * 0.56, h * 0.84), h * 0.05, SMALL_LIT, SMALL_DARK)
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
            d.text((tx + off, cy + off), line, font=f, fill=(10, 8, 40),
                   stroke_width=sw, stroke_fill=(10, 8, 40))
        d.text((tx, cy), line, font=f, fill=(255, 255, 255), stroke_width=sw, stroke_fill=(52, 30, 120))
        cy += (bbox[3] - bbox[1]) * 1.12
    return cy


def boot(w, h):
    W, H = w * SS, h * SS
    img = scene(W, H, (int(W * 0.80), int(H * 0.26)), int(H * 0.12))
    s = H / 720
    bottom = text(img, ["Super Mario", "Galaxy"], int(W * 0.06), int(H * 0.13),
                  [int(104 * s), int(136 * s)])
    sub = ImageFont.truetype(BOLD, int(40 * s))
    d = ImageDraw.Draw(img)
    d.text((int(W * 0.068), int(bottom + 18 * s)), "Wii U port", font=sub, fill=(255, 240, 170),
           stroke_width=int(4 * s), stroke_fill=(52, 30, 120))
    return img.resize((w, h), Image.LANCZOS)


def icon():
    W = H = 128 * SS
    img = scene(W, H, (int(W * 0.50), int(H * 0.26)), int(H * 0.17), planet=False)
    text(img, ["Mario", "Galaxy"], W // 2, int(H * 0.50), [int(H * 0.17), int(H * 0.17)],
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
