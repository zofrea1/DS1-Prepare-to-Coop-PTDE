"""Builds assets/TeamDied.png: the "YOU DIED" band of the game's death text, re-lettered "YOUR TEAM DIED".

The death text is no string: it is a 1024 x 100 band of the English menu atlas "Menu14" (menu/english/menu_local.tpf,
rows 496-595, the sprite main_text_youdied of menu.drb's F23_YouDied and F23_YouDide_phantom). The band is opaque:
dark red letters on black. This tool reads the atlas from the game's own archives and letters "YOUR TEAM DIED" in the
same type: Y, O, U, D, I and E are the band's own letters; R, T and A come from "TARGET DESTROYED" and M from
"BLACK PHANTOM DESTROYED" (same typeface, crisp white with alpha), scaled to the band's size and given its colour.
The scale and the colour are measured on the letters both lines share (D, E, O, Y).

usage: build_team_died.py <PTDE DATA folder> [preview.png]
Writes assets/TeamDied.png and src/coop/team_died_art.inc (the PNG built into the DLL); rebuild afterwards.
Needs numpy, scipy and Pillow.
"""
import io
import os
import struct
import sys
import zlib

import numpy as np
from PIL import Image
from scipy import ndimage

BAND = (496, 596)  # rows of the death text in Menu14
TEXT = 'YOUR TEAM DIED'
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_PNG = os.path.join(ROOT, 'assets', 'TeamDied.png')
OUT_INC = os.path.join(ROOT, 'src', 'coop', 'team_died_art.inc')


# ---------------------------------------------------------------- the game's archives
def bhd5_hash(path):
    v = 0
    for c in path.lower():
        v = (v * 37 + ord(c)) & 0xFFFFFFFF
    return v


def read_archived(data_dir, path):
    want = bhd5_hash(path)
    for n in range(4):
        bhd = open(os.path.join(data_dir, 'dvdbnd%d.bhd5' % n), 'rb').read()
        assert bhd[:4] == b'BHD5'
        buckets, boff = struct.unpack_from('<ii', bhd, 0x10)
        for b in range(buckets):
            cnt, off = struct.unpack_from('<ii', bhd, boff + b * 8)
            for i in range(cnt):
                h, size, fo = struct.unpack_from('<IIq', bhd, off + i * 16)
                if h == want:
                    with open(os.path.join(data_dir, 'dvdbnd%d.bdt' % n), 'rb') as f:
                        f.seek(fo)
                        return f.read(size)
    raise SystemExit('not in the archives: ' + path)


def undcx(blob):
    i = blob.find(b'DCA\x00')
    return zlib.decompress(blob[i + struct.unpack_from('>I', blob, i + 4)[0]:])


def tpf_texture(tpf, name):
    count = struct.unpack_from('<I', tpf, 8)[0]
    for k in range(count):
        off, size, _fmt, _typ, _mips, _flags, name_off, _unk = struct.unpack_from('<IIBBBBII', tpf, 0x10 + k * 0x14)
        if tpf[name_off:tpf.index(b'\x00', name_off)].decode() == name:
            return tpf[off:off + size]
    raise SystemExit('no texture ' + name)


# ---------------------------------------------------------------- letters
def columns(img, thr):
    on = img.max(axis=0) > thr
    out, start = [], None
    for x, c in enumerate(on):
        if c and start is None:
            start = x
        if not c and start is not None:
            out.append((start, x - 1))
            start = None
    if start is not None:
        out.append((start, len(on) - 1))
    return out


def rows_of(img, thr):
    r = np.where(img.max(axis=1) > thr)[0]
    return int(r.min()), int(r.max())


def crisp(atlas, y0, y1, x0, x1):
    """A crisp letter's coverage (0..1), cropped to its ink."""
    a = atlas[y0:y1 + 1, x0:x1 + 1, 3].astype(np.float64) / 255.0
    t, b = rows_of(a, 0.08)
    return a[t:b + 1], t


def scaled(cov, sx, sy):
    h, w = cov.shape
    im = Image.fromarray(cov.astype(np.float32), mode='F').resize((max(1, round(w * sx)), max(1, round(h * sy))),
                                                                   Image.LANCZOS)
    return np.clip(np.asarray(im).astype(np.float64), 0, 1)


def render(cov, gain, sigma):
    out = ndimage.gaussian_filter(cov, sigma) if sigma > 0 else cov
    return np.clip(out * gain, 0, 255)


def placed(c, target):
    """c laid on the target's frame at the offset (around the centre of mass) that matches it best."""
    h, w = target.shape
    cy, cx = ndimage.center_of_mass(target)
    ky, kx = ndimage.center_of_mass(c)
    best = None
    for dy in range(-2, 3):
        for dx in range(-2, 3):
            canvas = np.zeros((h + 60, w + 60))
            oy, ox = int(round(cy - ky)) + 30 + dy, int(round(cx - kx)) + 30 + dx
            if oy < 0 or ox < 0 or oy + c.shape[0] > canvas.shape[0] or ox + c.shape[1] > canvas.shape[1]:
                continue
            canvas[oy:oy + c.shape[0], ox:ox + c.shape[1]] = c
            f = canvas[30:30 + h, 30:30 + w]
            score = (f * target).sum() / max(1e-9, np.sqrt((f * f).sum()))
            if best is None or score > best[0]:
                best = (score, f)
    return best[1]


def fit(pairs, sx_range, sy_range):
    """Scale (across and up apart: the death text is wider set), blur and gain that turn the crisp letters into the
    band's, least squares over the letters both have."""
    best = None
    for sx in sx_range:
        for sy in sy_range:
            for sigma in (0.0, 0.4, 0.7, 1.0):
                rendered = []
                for cov, target in pairs:
                    c = scaled(cov, sx, sy)
                    c = ndimage.gaussian_filter(c, sigma) if sigma > 0 else c
                    rendered.append((placed(c, target), target))
                num = sum((c * t).sum() for c, t in rendered)
                den = sum((c * c).sum() for c, t in rendered)
                gain = num / den
                err = sum(((c * gain - t) ** 2).sum() for c, t in rendered)
                n = sum(t.size for c, t in rendered)
                if best is None or err < best[0]:
                    best = (err, sx, sy, sigma, gain, np.sqrt(err / n))
    return best


def main():
    data_dir = sys.argv[1]
    preview = sys.argv[2] if len(sys.argv) > 2 else None
    out_png = OUT_PNG
    tpf = undcx(read_archived(data_dir, '/menu/english/menu_local.tpf.dcx'))
    atlas = np.asarray(Image.open(io.BytesIO(tpf_texture(tpf, 'Menu14'))).convert('RGBA')).astype(np.float64)

    band = atlas[BAND[0]:BAND[1]]
    red = band[:, :, 0]
    assert band[:, :, 3].min() == 255, 'the death text band is not opaque: another language or version?'
    spans = columns(red, 6)
    assert len(spans) == 7, spans  # Y O U  D I E D
    own = dict(zip('YOUDIED', spans))  # the second D wins; both are the same letter
    rgb_per_red = np.array([1.0, (band[:, :, 1][red > 30] / red[red > 30]).mean(),
                            (band[:, :, 2][red > 30] / red[red > 30]).mean()])

    # crisp sources: TARGET DESTROYED (rows 604-677) and BLACK PHANTOM DESTROYED (rows 788-857)
    target_cols = columns(atlas[604:678, :, 3], 20)
    assert len(target_cols) == 15, target_cols
    target = dict(zip('TARGETDESTROYED', target_cols))
    target.update({'T': target_cols[0], 'A': target_cols[1], 'R': target_cols[2], 'D': target_cols[6],
                   'E': target_cols[4], 'O': target_cols[11], 'Y': target_cols[12]})
    phantom_cols = columns(atlas[788:858, :, 3], 20)
    m_span = phantom_cols[10]  # B L A C K  P H A N T O M ... (B+L touch, so the 11th run is M)

    def own_letter(ch):
        x0, x1 = own[ch]
        return red[:, x0:x1 + 1]

    def own_ink(ch):
        t = own_letter(ch)
        t0, t1 = rows_of(t, 6)
        return t[t0:t1 + 1]

    target_pairs = [(crisp(atlas, 604, 677, *target[ch])[0], own_ink(ch)) for ch in 'DEOY']
    phantom_spans = {'D': phantom_cols[11], 'E': phantom_cols[12], 'O': phantom_cols[9]}
    phantom_pairs = [(crisp(atlas, 788, 857, *phantom_spans[ch])[0], own_ink(ch)) for ch in 'DEO']
    fits = {}
    for name, pairs, sx0 in (('target', target_pairs, 1.40), ('phantom', phantom_pairs, 1.70)):
        err, sx, sy, sigma, gain, rms = fit(pairs, np.arange(sx0 - 0.04, sx0 + 0.045, 0.01),
                                            np.arange(1.22, 1.345, 0.01))
        fits[name] = (sx, sy, sigma, gain)
        print('fit %s: across %.2f, up %.2f, blur %.1f, gain %.1f (rms %.1f of 0-%d)' % (name, sx, sy, sigma, gain,
                                                                                         rms, int(red.max())))

    baseline = rows_of(own_letter('D'), 6)[1]  # the flat bottom of D, E and I
    top = rows_of(own_letter('D'), 6)[0]

    def made(ch, y0, y1, span, source):
        sx, sy, sigma, gain = fits[source]
        cov, _ = crisp(atlas, y0, y1, *span)
        c = render(scaled(cov, sx, sy), gain, sigma)
        t0, t1 = rows_of(c, 6)
        c0, c1 = columns(c, 0)[0][0], columns(c, 0)[-1][1]
        return c[t0:t1 + 1, c0:c1 + 1]

    glyphs = {}
    for ch in 'YOUDIE':
        g = own_letter(ch)
        t0, t1 = rows_of(g, 0)
        glyphs[ch] = (g[t0:t1 + 1], t0)
    for ch in 'RTA':
        glyphs[ch] = (made(ch, 604, 677, target[ch], 'target'), None)
    glyphs['M'] = (made('M', 788, 857, m_span, 'phantom'), None)
    for ch in 'RTAM':
        print('  %s: %d x %d (D is %d x %d)' % (ch, glyphs[ch][0].shape[1], glyphs[ch][0].shape[0],
                                                own['D'][1] - own['D'][0] + 1, baseline - top + 1))

    # spacing: the band's own gaps between letters and between the words
    gaps = [own['O'][0] - own['Y'][1] - 1, own['U'][0] - own['O'][1] - 1, own['E'][0] - spans[4][1] - 1]
    letter_gap = int(round(np.mean(gaps)))
    word_gap = spans[3][0] - spans[2][1] - 1

    pieces = []
    for word_i, word in enumerate(TEXT.split(' ')):
        if word_i:
            pieces.append(('gap', word_gap))
        for i, ch in enumerate(word):
            if i:
                pieces.append(('gap', letter_gap))
            pieces.append(('glyph', ch))
    width = sum(v if kind == 'gap' else glyphs[v][0].shape[1] for kind, v in pieces)
    centre = (spans[0][0] + spans[-1][1]) / 2.0
    x = int(round(centre - width / 2.0))
    assert x >= 16 and x + width <= 1024 - 16, ('does not fit', x, width)

    out = np.zeros((BAND[1] - BAND[0], 1024))
    for kind, v in pieces:
        if kind == 'gap':
            x += v
            continue
        g, t0 = glyphs[v]
        if t0 is None:
            t0 = baseline - g.shape[0] + 1  # sit on the baseline
        h, w = g.shape
        out[t0:t0 + h, x:x + w] = np.maximum(out[t0:t0 + h, x:x + w], g)
        x += w

    rgba = np.zeros((out.shape[0], 1024, 4), np.uint8)
    rgba[:, :, :3] = np.clip(out[:, :, None] * rgb_per_red[None, None, :] + 0.5, 0, 255).astype(np.uint8)
    rgba[:, :, 3] = 255
    Image.fromarray(rgba, 'RGBA').save(out_png, optimize=True)
    print('%s: %d px wide (YOU DIED %d), letter gap %d, word gap %d' % (out_png, width, spans[-1][1] - spans[0][0] + 1,
                                                                        letter_gap, word_gap))
    data = open(out_png, 'rb').read()
    lines = ['// Generated by tools/build_team_died.py (assets/TeamDied.png). Do not edit.',
             'const unsigned char kTeamDiedArt[%d] = {' % len(data)]
    for i in range(0, len(data), 24):
        lines.append('    ' + ', '.join('0x%02X' % b for b in data[i:i + 24]) + ',')
    lines.append('};')
    open(OUT_INC, 'w', newline='\n').write('\n'.join(lines) + '\n')
    print('%s: %d bytes of PNG' % (OUT_INC, len(data)))
    if preview:
        both = np.vstack([np.clip(red * 2.5, 0, 255), np.zeros((8, 1024)), np.clip(out * 2.5, 0, 255)])
        Image.fromarray(both.astype(np.uint8)).save(preview)


if __name__ == '__main__':
    main()
