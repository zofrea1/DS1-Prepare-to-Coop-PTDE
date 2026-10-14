"""Makes assets/ptde/PrepareToCoop-Title.wmv, the title movie with PREPARE TO CO-OP EDITION, from the game's
movWW/frpg_title.wmv and the Prepare to Co-op logo texture. Run through tools/build_title_movie.bat.

The title movie (1280x720, 335 frames at 29.97 fps) zooms the DARK SOULS logo in and then fades the edition line
in under it; when it ends the menu draws the same picture from the logo texture "Title" (see src/ptde/title.cpp),
1:1 in 720p: the texture's first edition line "PREPARE TO" lands at +129,+80 and the second, "DIE(tm) EDITION",
at +477,+25. The edition line is absent before frame 151; from there it fades in (brightness 0 to 1 by frame
~211), sharpens (a blur of ~2.8 px down to none by ~210) and shrinks into place (3.2% larger and 3.5 px higher
at first, still from frame 245 on). tools/title_movie_fit.json holds those per-frame values, fitted from the
original by --refit.

Each frame's second line is repainted: the old line (taken from the movie's last frame, moved, blurred and faded
the same way) is taken out, the background under it is filled in from the pixels around it so no trace of the
old letters is left, and the new texture's second line is put in with the same movement, blur and fade. The
first line and everything else stay the original pixels. The frames are then encoded by tools/wmvenc.exe.

usage: title_movie.py --movie <frpg_title.wmv> --encoder <wmvenc.exe> [--ffmpeg <ffmpeg.exe>] [--refit]
"""
import argparse
import glob
import json
import os
import shutil
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image
from scipy import ndimage, optimize
from scipy.signal import savgol_filter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, 'assets', 'ptde')
FIT = os.path.join(ROOT, 'tools', 'title_movie_fit.json')

FRAMES, WIDTH, HEIGHT = 335, 1280, 720
LINE1 = (129, 80)                         # texture -> movie offset of "PREPARE TO"
LINE2 = (477, 25)                         # texture -> movie offset of the second line
X0, Y0, X1, Y1 = 260, 395, 1020, 490      # the part of the frame that is worked on (around the edition line)
CX, CY = 640.0 - X0, 441.0 - Y0           # zoom centre (the screen's middle column, the line's middle row)
BX0, BY0, BX1, BY1 = 641, 411, 990, 472   # the second line in the movie's still frames


def find_ffmpeg(given):
    if given:
        return given
    try:
        import imageio_ffmpeg
        return imageio_ffmpeg.get_ffmpeg_exe()
    except ImportError:
        path = shutil.which('ffmpeg')
        if path:
            return path
    sys.exit('ffmpeg not found: pip install imageio-ffmpeg, or pass --ffmpeg')


def decode(ffmpeg, movie, outdir):
    os.makedirs(outdir, exist_ok=True)
    subprocess.run([ffmpeg, '-hide_banner', '-loglevel', 'error', '-i', movie, '-fps_mode', 'passthrough',
                    os.path.join(outdir, 'f%04d.png')], check=True)
    frames = sorted(glob.glob(os.path.join(outdir, 'f*.png')))
    if len(frames) != FRAMES:
        sys.exit('expected %d frames in %s, got %d' % (FRAMES, movie, len(frames)))
    return frames


def load(path):
    return np.asarray(Image.open(path).convert('RGB')).astype(float)


def calibrate(still, old_tex):
    """Colour gain from texture to movie, measured on both edition lines of the original."""
    gains, worst = [], 0.0
    for (dx, dy), (tx0, ty0, tx1, ty1) in [(LINE1, (175, 338, 505, 384)), (LINE2, (170, 393, 505, 439))]:
        t = old_tex[ty0:ty1, tx0:tx1]
        f = still[ty0 + dy:ty1 + dy, tx0 + dx:tx1 + dx]
        g = [float((t[..., c] * f[..., c]).sum() / (t[..., c] ** 2).sum()) for c in range(3)]
        gains.append(g)
        worst = max(worst, float(np.abs(f - t * np.array(g)).mean()))
    if worst > 6:
        sys.exit('the movie does not line up with the logo texture (mean difference %.1f); not this game\'s movie?' % worst)
    return np.mean(np.array(gains), axis=0)


class Warp:
    """Moves, scales and blurs images of the work region the way the movie animates the edition line."""

    def __init__(self):
        self.h, self.w = Y1 - Y0, X1 - X0
        self.yy, self.xx = np.mgrid[0:self.h, 0:self.w].astype(float)
        self.cache = {}

    def coeffs(self, img):
        key = id(img)
        if key not in self.cache:
            planes = [img[..., c] for c in range(img.shape[2])] if img.ndim == 3 else [img]
            self.cache[key] = (img, [ndimage.spline_filter(p, order=3) for p in planes])
        return self.cache[key][1]

    def __call__(self, img, ax, ay, bx, by, sigma):
        qx = ax * (self.xx - CX) + CX + bx
        qy = ay * (self.yy - CY) + CY + by
        out = []
        for cf in self.coeffs(img):
            w = ndimage.map_coordinates(cf, [qy, qx], order=3, mode='constant', cval=0.0, prefilter=False)
            out.append(ndimage.gaussian_filter(w, sigma, truncate=4.0) if sigma > 0.02 else w)
        return np.stack(out, 2) if img.ndim == 3 else out[0]


def refit(frames):
    """Fits, per frame, the movement, blur and brightness of the edition line against the still last frame."""
    still = load(frames[-1])[Y0:Y1, X0:X1]
    ref = still.mean(axis=2)
    planes = [still[..., c].copy() for c in range(3)]
    warp = Warp()
    rows = slice(412 - Y0, 472 - Y0)
    h, w = ref.shape
    yy, xx = warp.yy[rows], warp.xx[rows]
    basis = [np.ones_like(yy), yy / h - 0.5, xx / w - 0.5, (yy / h - 0.5) ** 2]  # smooth background (logo glow)

    def project(target, model):
        a = np.stack([model[rows].ravel()] + [b.ravel() for b in basis], 1)
        coef, *_ = np.linalg.lstsq(a, target[rows].ravel(), rcond=None)
        return coef, target[rows].ravel() - a @ coef

    fit = {}
    p = np.array([1.0, 1.0, 0.0, 0.0, 0.0])
    for i in range(260, 139, -1):  # backwards from the still frames, each fit starting from the next frame's
        frame = load(frames[i])[Y0:Y1, X0:X1]
        lum = frame.mean(axis=2)
        r = optimize.least_squares(lambda q: project(lum, warp(ref, *q))[1][::2], p,
                                   bounds=([0.85, 0.85, -20, -20, 0.0], [1.1, 1.1, 20, 20, 8.0]),
                                   x_scale=[0.002, 0.002, 0.1, 0.1, 0.1], diff_step=1e-4)
        p = r.x
        alpha = [float(project(frame[..., c], warp(planes[c], *p))[0][0]) for c in range(3)]
        fit[i] = list(map(float, p)) + alpha
        print('fit frame %3d: scale %.4f %.4f shift %+.2f %+.2f blur %.2f brightness %.3f' % (i, 1 / p[0], 1 / p[1], p[2], p[3], p[4], alpha[1]))
    first, still_from = 151, 245  # the line is absent before 151 (its fitted brightness < 0.01) and still from 245
    t = np.arange(first, still_from)
    cols = {k: np.array([fit[i][j] for i in t]) for j, k in enumerate(['ax', 'ay', 'bx', 'by', 'sigma', 'r', 'g', 'b'])}
    smooth = {}
    for k, v in cols.items():
        if k in ('ax', 'ay', 'bx', 'by'):  # straight lines ending at rest (scale 1, no shift) on frame 245
            rest = 1.0 if k in ('ax', 'ay') else 0.0
            slope = ((v - rest) * (t - still_from)).sum() / ((t - still_from) ** 2).sum()
            smooth[k] = rest + slope * (t - still_from)
        else:
            smooth[k] = np.clip(savgol_filter(v, 11, 2), 0, None)
    sig = smooth['sigma']
    sig[np.argmax((t > 200) & (sig < 0.12)):] = 0.0
    table = [[int(i)] + [round(float(smooth[k][j]), 5) for k in ['ax', 'ay', 'bx', 'by', 'sigma', 'r', 'g', 'b']]
             for j, i in enumerate(t)]
    write_fit(first, still_from, table)


def write_fit(first, still_from, table):
    """One frame per line: frame, x and y sampling scale, x and y shift, blur sigma, r g b brightness."""
    lines = ['{"first": %d, "still": %d,' % (first, still_from),
             ' "columns": ["frame", "ax", "ay", "bx", "by", "sigma", "r", "g", "b"],',
             ' "rows": [']
    lines += ['  %s%s' % (json.dumps(r), ',' if k + 1 < len(table) else '') for k, r in enumerate(table)]
    lines.append(']}')
    open(FIT, 'w', newline='\n').write('\n'.join(lines) + '\n')
    print('wrote', FIT)


def repaint_all(frames, new_tex, gain, outdir):
    tab = json.load(open(FIT))
    rows = {r[0]: r[1:] for r in tab['rows']}
    first, still_from = tab['first'], tab['still']
    still = load(frames[-1])
    region = still[Y0:Y1, X0:X1]
    old2 = np.zeros_like(region)  # the old second line alone
    old2[BY0 - Y0:BY1 - Y0, BX0 - X0:BX1 - X0] = region[BY0 - Y0:BY1 - Y0, BX0 - X0:BX1 - X0]
    old1 = region - old2          # everything else, "PREPARE TO" included
    new2 = np.zeros_like(region)  # the new texture's second line where the menu will draw it
    tx0, tx1 = BX0 - LINE2[0], min(BX1 - LINE2[0], new_tex.shape[1])
    ty0, ty1 = BY0 - LINE2[1], BY1 - LINE2[1]
    new2[BY0 - Y0:BY1 - Y0, BX0 - X0:BX0 - X0 + (tx1 - tx0)] = new_tex[ty0:ty1, tx0:tx1] * gain
    # where the old letters are (a little wider), for filling in the background under them
    support = ndimage.gaussian_filter(ndimage.binary_dilation(old2.max(axis=2) > 4, iterations=2).astype(float), 0.8)
    warp = Warp()
    os.makedirs(outdir, exist_ok=True)
    for i, path in enumerate(frames):
        frame = np.asarray(Image.open(path).convert('RGB'))
        if i >= first:
            p = rows[i] if i < still_from else [1.0, 1.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0]
            move, alpha = p[:5], np.array(p[5:8])
            src = frame[Y0:Y1, X0:X1].astype(float)
            line1 = warp(old1, *move) * alpha
            rest = src - line1 - warp(old2, *move) * alpha  # the frame without its edition line
            w = np.clip(warp(support, *move) * 1.5, 0, 1)
            keep = 1.0 - w
            den = np.maximum(ndimage.gaussian_filter(keep, 5.0), 1e-3)
            fill = np.stack([ndimage.gaussian_filter(rest[..., c] * keep, 5.0) / den for c in range(3)], 2)
            out = warp(new2, *move) * alpha + line1 + keep[..., None] * rest + w[..., None] * fill
            frame = frame.copy()
            frame[Y0:Y1, X0:X1] = np.clip(np.rint(out), 0, 255).astype(np.uint8)
        Image.fromarray(frame).save(os.path.join(outdir, 'f%04d.png' % (i + 1)))
    return first, still_from


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--movie', required=True, help="the game's movWW/frpg_title.wmv")
    ap.add_argument('--encoder', required=True, help='tools/wmvenc.exe, built by build_title_movie.bat')
    ap.add_argument('--ffmpeg', help='ffmpeg.exe for decoding (default: imageio-ffmpeg, then PATH)')
    ap.add_argument('--old', default=os.path.join(ASSETS, 'Title-original-PTDE.png'))
    ap.add_argument('--new', default=os.path.join(ASSETS, 'PrepareToCoop-Title.png'))
    ap.add_argument('--out', default=os.path.join(ASSETS, 'PrepareToCoop-Title.wmv'))
    ap.add_argument('--refit', action='store_true', help='fit the animation again (a few minutes) into ' + FIT)
    ap.add_argument('--keep', help='keep the frames in this folder instead of a temporary one')
    args = ap.parse_args()
    ffmpeg = find_ffmpeg(args.ffmpeg)
    work = args.keep or tempfile.mkdtemp(prefix='ptc-title-')
    try:
        frames = decode(ffmpeg, args.movie, os.path.join(work, 'original'))
        old_tex = load(args.old)
        new_tex = load(args.new)
        if new_tex.shape[:2] != (512, 512):
            sys.exit('%s must be 512 x 512' % args.new)
        gain = calibrate(load(frames[-1]), old_tex)
        print('texture -> movie colour gain', np.round(gain, 3))
        if args.refit:
            refit(frames)
        first, still_from = repaint_all(frames, new_tex, gain, os.path.join(work, 'new'))
        print('repainted frames %d-%d (the line moves until %d)' % (first + 1, FRAMES, still_from))
        subprocess.run([args.encoder, os.path.join(work, 'new', 'f%04d.png'), str(FRAMES), args.out], check=True)
        print('wrote %s (%d bytes)' % (args.out, os.path.getsize(args.out)))
    finally:
        if not args.keep:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == '__main__':
    main()
