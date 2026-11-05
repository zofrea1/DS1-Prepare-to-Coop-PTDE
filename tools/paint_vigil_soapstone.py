"""Paints the Vigil Soapstone's item icon from the game's own White Sign Soapstone: the same white stone, turned a
cooler ash, its gold cord in the Death Knell's hemp, and two barred feathers of the bell's tail knotted to the cord's
loose end (as on the Death Knell's icon, tools/paint_death_knell.py). 80 x 90, as the game's goods icons.

The White Sign Soapstone's icon (EquIcon_2000, its iconId) is the 80 x 90 cell (1, 1)-(81, 91) of the atlas "Icon20"
(menu/menu_0.tpf, 1024 x 1024 DXT5); this tool reads it from the game's own archives.

usage: paint_vigil_soapstone.py <PTDE DATA folder> [preview.png]
Writes assets/VigilSoapstone.png; then run build_icons.py and rebuild. Needs numpy and Pillow.
"""
# SPDX-License-Identifier: GPL-3.0-or-later
import io
import math
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from build_team_died import read_archived, tpf_texture, undcx  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'assets', 'VigilSoapstone.png')
CELL = (1, 1, 81, 91)  # EquIcon_2000 in Icon20
S = 4  # the feathers are painted at 4x and scaled down
HEMP = np.array([87, 72, 57]) / 255.0  # the Death Knell's cord


def smooth(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0, 1)
    return t * t * (3 - 2 * t)


def over(dst, src):
    """Alpha-composites float RGBA src over dst (straight alpha, 0..1)."""
    a = src[..., 3:4]
    out = dst.copy()
    out[..., :3] = src[..., :3] * a + dst[..., :3] * dst[..., 3:4] * (1 - a)
    out[..., 3:4] = a + dst[..., 3:4] * (1 - a)
    nz = out[..., 3:4] > 1e-6
    out[..., :3] = np.where(nz, out[..., :3] / np.maximum(out[..., 3:4], 1e-6), 0)
    return out


def recolour(src):
    """The cord from gold to hemp (its light and shade kept), the stone a little cooler and paler."""
    h, w = src.shape[:2]
    rgb = src[..., :3]
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    mx, mn = rgb.max(-1), rgb.min(-1)
    sat = np.where(mx > 1e-6, (mx - mn) / np.maximum(mx, 1e-6), 0)
    hue = np.degrees(np.arctan2(math.sqrt(3) * (g - b), 2 * r - g - b)) % 360
    lum = 0.299 * r + 0.587 * g + 0.114 * b
    golden = smooth(0.18, 0.38, sat) * smooth(8, 18, hue) * (1 - smooth(55, 70, hue))
    # The cord binds the stone's head and hangs loose to the left; lower down the same hue is the stone's speckle.
    yy, xx = np.mgrid[0:h, 0:w].astype(float)
    region = np.maximum(1 - smooth(42, 48, yy), (1 - smooth(28, 33, xx)) * (1 - smooth(64, 68, yy)))
    cord_w = golden * region
    speck_w = golden * (1 - region)
    cord = HEMP[None, None, :] * np.clip(lum[..., None] / 0.40, 0, 3) ** 1.15 * 0.856
    stone = rgb * np.array([0.95, 0.985, 1.04])
    stone = stone + (1 - stone) * 0.04
    ash = lum[..., None] * np.array([0.97, 0.99, 1.03])
    stone = stone * (1 - 0.75 * speck_w[..., None]) + ash * 0.75 * speck_w[..., None]
    out = cord * cord_w[..., None] + stone * (1 - cord_w[..., None])
    return np.concatenate([np.clip(out, 0, 1), src[..., 3:4]], -1)


def bez(p0, p1, p2, t):
    a, b, c = (1 - t) ** 2, 2 * (1 - t) * t, t * t
    return (a * p0[0] + b * p1[0] + c * p2[0], a * p0[1] + b * p1[1] + c * p2[1])


def feather(img, p0, p1, p2, width, seed, light=1.0):
    """A barred feather along a quadratic curve, as the Death Knell's (fewer bars: it is shorter)."""
    r = np.random.default_rng(seed)
    d = ImageDraw.Draw(img)
    n = 160
    pts = [bez(p0, p1, p2, i / n) for i in range(n + 1)]
    for i in range(3, n + 1):
        t = i / n
        px, py = pts[i]
        qx, qy = pts[i - 1]
        dx, dy = px - qx, py - qy
        ln = math.hypot(dx, dy) + 1e-9
        dx, dy = dx / ln, dy / ln
        w = width * min(1.0, 0.25 + t * 3.0) * (1.0 - 0.7 * t ** 2.5)
        bar = 0.5 + 0.5 * math.sin(t * 30 + seed * 1.7)
        for side in (-1, 1):
            if r.random() < 0.10:
                continue  # tattered
            ang = side * (0.62 + r.normal(0, 0.10))
            ca, sa = math.cos(ang), math.sin(ang)
            vx, vy = dx * ca - dy * sa, dx * sa + dy * ca
            ll = w * (0.75 + r.random() * 0.45)
            mx, my = px + vx * ll * 0.55 + dx * ll * 0.12, py + vy * ll * 0.55 + dy * ll * 0.12
            ex, ey = px + vx * ll + dx * ll * 0.35, py + vy * ll + dy * ll * 0.35
            if bar > 0.58:
                c = (150 + r.random() * 55) * light
                col = (int(c), int(c * 0.98), int(c * 0.94), 235)
            else:
                c = (32 + r.random() * 26) * light
                col = (int(c * 1.05), int(c * 0.95), int(c * 0.86), 240)
            d.line([(px, py), (mx, my), (ex, ey)], fill=col, width=2)
    d.line(pts, fill=(int(210 * light), int(204 * light), int(190 * light), 255), width=3)


def feathers(w, h):
    """The two feathers and the knot at the cord's end (8, 61), with their shadow, at 1x."""
    big = Image.new('RGBA', (w * S, h * S), (0, 0, 0, 0))
    root = (8 * S, 61 * S)
    feather(big, root, (-2 * S, 74 * S), (10 * S, 88 * S), 22, 5, 0.95)
    feather(big, root, (6 * S, 76 * S), (20 * S, 86 * S), 17, 9, 0.8)
    d = ImageDraw.Draw(big)
    d.ellipse([root[0] - 9, root[1] - 9, root[0] + 9, root[1] + 9], fill=(48, 38, 28, 255))
    d.ellipse([root[0] - 6, root[1] - 7, root[0] + 5, root[1] + 4], fill=(96, 80, 62, 255))
    big = big.filter(ImageFilter.GaussianBlur(0.8))
    fe = np.asarray(big.resize((w, h), Image.LANCZOS)).astype(np.float64) / 255.0
    sh = Image.new('L', (w, h), 0)
    ImageDraw.Draw(sh).ellipse([2, 84, 24, 89], fill=120)
    shadow = np.zeros((h, w, 4))
    shadow[..., 3] = np.asarray(sh.filter(ImageFilter.GaussianBlur(1.5))).astype(np.float64) / 255.0 * 0.6
    return over(shadow, fe)


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    tpf = undcx(read_archived(sys.argv[1], '/menu/menu_0.tpf.dcx'))
    atlas = Image.open(io.BytesIO(tpf_texture(tpf, 'Icon20'))).convert('RGBA')
    soap = np.asarray(atlas.crop(CELL)).astype(np.float64) / 255.0
    h, w = soap.shape[:2]
    icon = over(feathers(w, h), recolour(soap))
    img = Image.fromarray((np.clip(icon, 0, 1) * 255 + 0.5).astype(np.uint8), 'RGBA')
    img.save(OUT)
    print('wrote', OUT)
    if len(sys.argv) > 2:
        sheet = Image.new('RGBA', (w * 2 + 10, h), (40, 40, 40, 255))
        sheet.alpha_composite(Image.fromarray((soap * 255 + 0.5).astype(np.uint8), 'RGBA'), (0, 0))
        sheet.alpha_composite(img, (w + 10, 0))
        sheet.resize((sheet.width * 4, sheet.height * 4), Image.NEAREST).save(sys.argv[2])


if __name__ == '__main__':
    main()
