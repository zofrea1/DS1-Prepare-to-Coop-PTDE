"""Paints the Death Knell's item icon: an old iron bell on a frayed cord with a tail of barred feathers, in the manner of
the game's own goods icons (80 x 90, a soft shadow at the lower right). Painted at 4x and scaled down.

usage: paint_death_knell.py <out.png> [preview.png]   (the shipped art is assets/DeathKnell.png)
Then run build_death_knell_icon.py and rebuild. Needs numpy, scipy and Pillow.
"""
# SPDX-License-Identifier: GPL-3.0-or-later
import math
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter
from scipy.ndimage import gaussian_filter, zoom

S = 4
W, H = 80 * S, 90 * S
rng = np.random.default_rng(1431)


def noise(scale, octaves=4, seed=0):
    r = np.random.default_rng(seed)
    out = np.zeros((H, W))
    amp = 1.0
    total = 0
    for o in range(octaves):
        cell = max(2, int(scale / (2 ** o)))
        g = r.random((H // cell + 3, W // cell + 3))
        up = zoom(g, cell, order=3)[:H, :W]
        out += up * amp
        total += amp
        amp *= 0.5
    out /= total
    return (out - out.min()) / (out.max() - out.min() + 1e-9)


def over(dst, src):
    """Alpha-composites float RGBA src over dst (both H x W x 4, 0..1)."""
    a = src[..., 3:4]
    out = dst.copy()
    out[..., :3] = src[..., :3] * a + dst[..., :3] * dst[..., 3:4] * (1 - a)
    out[..., 3:4] = a + dst[..., 3:4] * (1 - a)
    nz = out[..., 3:4] > 1e-6
    out[..., :3] = np.where(nz, out[..., :3] / np.maximum(out[..., 3:4], 1e-6), 0)
    return out


def layer():
    return np.zeros((H, W, 4))


yy, xx = np.mgrid[0:H, 0:W].astype(float)
light = np.array([-0.55, -0.65, 0.52])
light /= np.linalg.norm(light)

canvas = layer()

# ---------------------------------------------------------------- shadow
sh = Image.new('L', (W, H), 0)
d = ImageDraw.Draw(sh)
d.ellipse([90, 286, 318, 346], fill=170)
d.ellipse([10, 312, 260, 356], fill=110)
sh = sh.filter(ImageFilter.GaussianBlur(14))
shadow = layer()
shadow[..., 3] = np.asarray(sh) / 255.0 * 0.85
canvas = over(canvas, shadow)

# ---------------------------------------------------------------- feathers (behind the bell)
def bez(p0, p1, p2, t):
    a = (1 - t) ** 2
    b = 2 * (1 - t) * t
    c = t * t
    return (a * p0[0] + b * p1[0] + c * p2[0], a * p0[1] + b * p1[1] + c * p2[1])


def feather(img, p0, p1, p2, width, seed, light=1.0):
    r = np.random.default_rng(seed)
    d = ImageDraw.Draw(img)
    n = 220
    pts = [bez(p0, p1, p2, i / n) for i in range(n + 1)]
    for i in range(3, n + 1):
        t = i / n
        px, py = pts[i]
        qx, qy = pts[i - 1]
        dx, dy = px - qx, py - qy
        ln = math.hypot(dx, dy) + 1e-9
        dx, dy = dx / ln, dy / ln
        w = width * min(1.0, 0.25 + t * 3.0) * (1.0 - 0.7 * t ** 2.5)
        bar = 0.5 + 0.5 * math.sin(t * 46 + seed * 1.7)
        for side in (-1, 1):
            if r.random() < 0.10:
                continue  # tattered
            ang = side * (0.62 + r.normal(0, 0.10))
            ca, sa = math.cos(ang), math.sin(ang)
            vx, vy = dx * ca - dy * sa, dx * sa + dy * ca
            L = w * (0.75 + r.random() * 0.45)
            # barbs curve toward the tip
            mx, my = px + vx * L * 0.55 + dx * L * 0.12, py + vy * L * 0.55 + dy * L * 0.12
            ex, ey = px + vx * L + dx * L * 0.35, py + vy * L + dy * L * 0.35
            if bar > 0.58:
                c = (140 + r.random() * 55) * light
                col = (int(c), int(c * 0.97), int(c * 0.91), 235)
            else:
                c = (32 + r.random() * 26) * light
                col = (int(c * 1.05), int(c * 0.95), int(c * 0.86), 240)
            d.line([(px, py), (mx, my), (ex, ey)], fill=col, width=2)
    d.line(pts, fill=(int(210 * light), int(204 * light), int(190 * light), 255), width=3)


fe = Image.new('RGBA', (W, H), (0, 0, 0, 0))
# a long tail sweeping from the knot under the bell, and two shorter ones
feather(fe, (100, 150), (10, 300), (150, 336), 34, 3, 0.85)
feather(fe, (104, 152), (40, 280), (250, 330), 30, 7, 1.0)
feather(fe, (98, 148), (30, 200), (40, 262), 24, 11, 0.75)
fe = fe.filter(ImageFilter.GaussianBlur(0.8))
fa = np.asarray(fe).astype(float) / 255.0
grime = noise(40, 4, 21)
fa[..., :3] *= (0.6 + 0.4 * grime[..., None])
canvas = over(canvas, fa)

# ---------------------------------------------------------------- cord: from the feathers' root up to the bell's eye
def rope(img, pts, width, seed):
    d = ImageDraw.Draw(img)
    r = np.random.default_rng(seed)
    # sample a smooth curve through pts (Catmull-Rom)
    P = [pts[0]] + pts + [pts[-1]]
    curve = []
    for i in range(1, len(P) - 2):
        p0, p1, p2, p3 = P[i - 1], P[i], P[i + 1], P[i + 2]
        for k in range(24):
            t = k / 24
            t2, t3 = t * t, t * t * t
            x = 0.5 * ((2 * p1[0]) + (-p0[0] + p2[0]) * t + (2 * p0[0] - 5 * p1[0] + 4 * p2[0] - p3[0]) * t2 +
                       (-p0[0] + 3 * p1[0] - 3 * p2[0] + p3[0]) * t3)
            y = 0.5 * ((2 * p1[1]) + (-p0[1] + p2[1]) * t + (2 * p0[1] - 5 * p1[1] + 4 * p2[1] - p3[1]) * t2 +
                       (-p0[1] + 3 * p1[1] - 3 * p2[1] + p3[1]) * t3)
            curve.append((x, y))
    curve.append(pts[-1])
    d.line(curve, fill=(34, 28, 22, 255), width=width + 4, joint='curve')
    d.line(curve, fill=(88, 74, 58, 255), width=width, joint='curve')
    # twist: short dark diagonals along the rope
    acc = 0
    for i in range(1, len(curve)):
        (ax, ay), (bx, by) = curve[i - 1], curve[i]
        seg = math.hypot(bx - ax, by - ay)
        acc += seg
        if acc >= 7:
            acc = 0
            dx, dy = (bx - ax) / (seg + 1e-9), (by - ay) / (seg + 1e-9)
            nx, ny = -dy, dx
            hw = width * 0.5
            c = int(58 + r.random() * 20)
            d.line([(bx + nx * hw - dx * 3, by + ny * hw - dy * 3), (bx - nx * hw + dx * 3, by - ny * hw + dy * 3)],
                   fill=(c, c - 12, c - 22, 255), width=2)
            d.line([(bx + nx * hw * 0.6 - dx * 1, by + ny * hw * 0.6 - dy * 1),
                    (bx + nx * hw * 0.1 + dx * 1, by + ny * hw * 0.1 + dy * 1)], fill=(132, 116, 92, 255), width=1)
    return curve


ro = Image.new('RGBA', (W, H), (0, 0, 0, 0))
# the loose end and fibres at the feathers' root
rope(ro, [(104, 152), (96, 118), (108, 82), (136, 58), (170, 50), (196, 58)], 11, 5)
dr = ImageDraw.Draw(ro)
for k in range(9):  # frayed fibres
    a = -2.2 + k * 0.18
    dr.line([(104, 150), (104 + math.cos(a) * (18 + k % 3 * 6), 150 + math.sin(a) * -(14 + k % 4 * 5))],
            fill=(128, 104, 74, 255), width=2)
# the knot binding the feathers
dr.ellipse([88, 134, 122, 170], fill=(60, 44, 30, 255))
dr.ellipse([92, 138, 116, 160], fill=(108, 84, 58, 255))
dr.arc([88, 134, 122, 170], 200, 320, fill=(140, 114, 84, 255), width=2)
ra = np.asarray(ro.filter(ImageFilter.GaussianBlur(0.5))).astype(float) / 255.0
canvas = over(canvas, ra)

# ---------------------------------------------------------------- the bell
cx, cy, R = 200, 178, 94
dx_, dy_ = (xx - cx) / R, (yy - cy) / R
r2 = dx_ ** 2 + dy_ ** 2
inside = r2 <= 1.0
nz = np.sqrt(np.clip(1 - r2, 0, 1))
dent = gaussian_filter(rng.random((H, W)), 6) - 0.5
N = np.stack([dx_ + dent * 1.4, dy_ + np.roll(dent, 9, 0) * 1.4, nz], -1)
N /= np.linalg.norm(N, axis=-1, keepdims=True)
lam = np.clip((N * light).sum(-1), 0, 1)
half = light + np.array([0, 0, 1.0])
half /= np.linalg.norm(half)
spec = np.clip((N * half).sum(-1), 0, 1) ** 18 * (0.35 + 0.65 * noise(10, 3, 71))
rim = np.clip(1 - nz, 0, 1) ** 3

tex = noise(26, 5, 31)
pat = noise(60, 3, 41)
pits = noise(6, 2, 51)
iron = np.array([0.24, 0.23, 0.215])
patina = np.array([0.26, 0.31, 0.27])  # a greenish bronze bloom
rust = np.array([0.40, 0.25, 0.15])
base = iron[None, None, :] * (0.75 + 0.5 * tex[..., None])
base = base * (1 - 0.45 * pat[..., None]) + patina[None, None, :] * 0.45 * pat[..., None]
rmask = np.clip((noise(18, 4, 61) - 0.50) * 3.0, 0, 1) * (0.5 + 0.5 * np.clip(dy_ + 0.3, 0, 1))
base = base * (1 - rmask[..., None]) + rust[None, None, :] * rmask[..., None]
grain = noise(3, 2, 81)
base *= (0.72 + 0.42 * pits[..., None]) * (0.8 + 0.4 * grain[..., None])

shade = 0.16 + 0.95 * lam
cav = np.clip(gaussian_filter(np.where(inside, 1.0, 0.0), 3), 0, 1)
shade = shade * (0.7 + 0.3 * cav)
col = base * shade[..., None] + 0.45 * spec[..., None] * np.array([0.95, 0.92, 0.85]) + 0.10 * rim[..., None] * np.array(
    [0.55, 0.6, 0.7])

# the raised band round the middle (a latitude a little above the equator, seen from slightly above)
lat = dy_ - 0.10 + 0.18 * nz * 0  # the band's height on the sphere
band_y = -0.06 + 0.22 * (1 - nz)  # tilt: the band curves down at the sides
bdist = np.abs(dy_ - band_y * 0.35 + 0.02)
band = inside & (bdist < 0.07)
band_hi = inside & (np.abs(dy_ - band_y * 0.35 - 0.05) < 0.022)
band_lo = inside & (np.abs(dy_ - band_y * 0.35 + 0.085) < 0.02)
col = np.where(band[..., None], col * 1.18 + 0.03, col)
col = np.where(band_hi[..., None], col * 0.55, col)
col = np.where(band_lo[..., None], col * 1.35 + 0.05, col)

# the slit: from the bottom up to the band, ending in a round hole each side, seen in perspective on the sphere
slit_w = 0.085 * (0.5 + 0.5 * nz)
slit = inside & (dy_ > 0.16) & (np.abs(dx_ + 0.10 * dy_) < slit_w)
holes = inside & ((((dx_ + 0.22) ** 2 + (dy_ - 0.20) ** 2) < 0.012) | (((dx_ - 0.02) ** 2 + (dy_ - 0.20) ** 2) < 0.012))
cut = slit | holes
col = np.where(cut[..., None], np.array([0.03, 0.025, 0.02]), col)
# lit lip under the cut (the edge facing the light)
lip = inside & ~cut & (np.roll(cut, 3, axis=0) | np.roll(cut, 3, axis=1))
col = np.where(lip[..., None], col * 1.5 + 0.05, col)
ao = inside & ~cut & (np.roll(cut, -3, axis=0) | np.roll(cut, -3, axis=1))
col = np.where(ao[..., None], col * 0.55, col)

scr = Image.new('L', (W, H), 0)
ds_ = ImageDraw.Draw(scr)
for k in range(14):
    a0 = rng.random() * 6.28
    x0 = cx + math.cos(a0) * R * rng.random() * 0.8
    y0 = cy + math.sin(a0) * R * rng.random() * 0.8
    a1 = rng.random() * 6.28
    ln_ = 10 + rng.random() * 26
    ds_.line([(x0, y0), (x0 + math.cos(a1) * ln_, y0 + math.sin(a1) * ln_)], fill=int(90 + rng.random() * 120), width=1)
scr_a = np.asarray(scr).astype(float) / 255.0 * inside * ~cut
col = col * (1 - 0.5 * scr_a[..., None]) + scr_a[..., None] * 0.5 * np.array([0.62, 0.6, 0.56])
bell = layer()
bell[..., :3] = np.clip(col, 0, 1)
edge = np.clip((1 - np.sqrt(r2)) * R / 1.5, 0, 1)
bell[..., 3] = np.where(inside, edge, 0)
# a dark outline, as the game's icons have
outline = (r2 <= (1 + 3.5 / R) ** 2) & ~inside
ol = layer()
ol[..., :3] = 0.05
ol[..., 3] = np.where(outline, 0.9, 0)
canvas = over(canvas, ol)
canvas = over(canvas, bell)

# the eye on top of the bell and the cord through it
eye = Image.new('RGBA', (W, H), (0, 0, 0, 0))
de = ImageDraw.Draw(eye)
ex, ey = 196, 82
de.ellipse([ex - 22, ey - 24, ex + 22, ey + 18], outline=(18, 16, 14, 255), width=13)
de.ellipse([ex - 22, ey - 24, ex + 22, ey + 18], outline=(86, 82, 74, 255), width=8)
de.arc([ex - 19, ey - 21, ex + 19, ey + 15], 190, 290, fill=(170, 164, 150, 255), width=3)
de.rectangle([ex - 12, ey + 10, ex + 12, ey + 22], fill=(40, 37, 33, 255))
de.rectangle([ex - 10, ey + 11, ex + 4, ey + 20], fill=(92, 88, 78, 255))
ea = np.asarray(eye.filter(ImageFilter.GaussianBlur(0.5))).astype(float) / 255.0
canvas = over(canvas, ea)
ro2 = Image.new('RGBA', (W, H), (0, 0, 0, 0))
rope(ro2, [(170, 50), (192, 54), (210, 70), (202, 90), (184, 82), (180, 66)], 9, 9)
canvas = over(canvas, np.asarray(ro2.filter(ImageFilter.GaussianBlur(0.5))).astype(float) / 255.0)

# ---------------------------------------------------------------- the whole: a little contrast, then down to 80 x 90
out = (np.clip(canvas, 0, 1) * 255).astype(np.uint8)
big = Image.fromarray(out, 'RGBA')
small = big.resize((80, 90), Image.LANCZOS)
# the game's icons are crisp: a light unsharp mask on the colour
rgb = small.convert('RGB').filter(ImageFilter.UnsharpMask(radius=1.2, percent=70, threshold=2))
small = Image.merge('RGBA', (*rgb.split(), small.split()[3]))
small.save(sys.argv[1])
if len(sys.argv) > 2:
    prev = Image.new('RGBA', (80 * 2 + 30, 90), (30, 28, 26, 255))
    prev.alpha_composite(small, (5, 0))
    prev = prev.resize((prev.width * 4, prev.height * 4), Image.NEAREST)
    big2 = Image.new('RGBA', big.size, (30, 28, 26, 255))
    big2.alpha_composite(big)
    sheet = Image.new('RGB', (prev.width + big2.width, max(prev.height, big2.height)), (30, 28, 26))
    sheet.paste(prev.convert('RGB'), (0, 0))
    sheet.paste(big2.convert('RGB'), (prev.width, 0))
    sheet.save(sys.argv[2])
