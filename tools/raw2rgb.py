#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Reference processor for SP2508 RAW10 frames using the stock tuning data.

Input: one V4L2_PIX_FMT_SBGGR10P frame (1616x1216, 2020 bytes per line, MIPI
RAW10: four MSB bytes then one byte holding the four 2-bit LSB pairs, pixel 0
in the low bits). Further frames in the file are ignored unless --frame is
given.

Pipeline (the order of the MediaTek ISP): crop to the 1600x1200 image area at
(2, 2), black level, defect pixel clamp, lens shading (stock spline tables),
white balance, bilinear demosaic, stock dynamic CCM, stock gamma (GGM).

usage: raw2rgb.py in.bin out.png [--no-lsc] [--no-awb] [--no-ccm] ...
Needs numpy and Pillow. Tables are read from data/sp2508/ (see --data).
"""

import argparse
import json
import math
import os
import sys

import numpy as np
from PIL import Image

WIDTH, HEIGHT, STRIDE = 1616, 1216, 2020
CROP_X, CROP_Y, CROP_W, CROP_H = 2, 2, 1600, 1200
WHITE = 1023

# Stock shading table selection: 0 below about 2800 K, 1 up to about 4900 K,
# 2 above. The CT of each AWB light source comes from the 3A NVRAM.
LSC_CT_INDEX = {'horizon': 0, 'a': 0, 'tl84': 1, 'cwf': 1, 'dnp': 2,
                'd65': 2, 'df': 2}


def load_data(path):
    def rd(name):
        with open(os.path.join(path, name)) as f:
            return json.load(f)
    return rd('isp_param.json'), rd('3a_param.json'), rd('lsc.json')


def unpack_raw10(path, frame):
    size = STRIDE * HEIGHT
    b = np.fromfile(path, np.uint8, count=size, offset=frame * size)
    if b.size != size:
        sys.exit('%s: short frame (%d of %d bytes)' % (path, b.size, size))
    b = b.reshape(HEIGHT, STRIDE // 5, 5)
    lsb = (b[:, :, 4:5] >> (2 * np.arange(4, dtype=np.uint8))) & 3
    raw = (b[:, :, :4].astype(np.uint16) << 2) | lsb
    return raw.reshape(HEIGHT, WIDTH)


def planes(img):
    """Split a BGGR mosaic into [B, Gb, Gr, R] planes."""
    return [img[0::2, 0::2], img[0::2, 1::2], img[1::2, 0::2], img[1::2, 1::2]]


def merge(p):
    h, w = p[0].shape
    out = np.empty((2 * h, 2 * w), p[0].dtype)
    out[0::2, 0::2], out[0::2, 1::2], out[1::2, 0::2], out[1::2, 1::2] = p
    return out


def defect_clamp(img):
    """Clamp each pixel to the range of its eight same-colour neighbours."""
    p = np.pad(img, 2, mode='reflect')
    h, w = img.shape
    nb = [p[2 + dy:2 + dy + h, 2 + dx:2 + dx + w]
          for dy in (-2, 0, 2) for dx in (-2, 0, 2) if dy or dx]
    st = np.stack(nb)
    return np.clip(img, st.min(0), st.max(0))


def lsc_gain_maps(lsc, table, ct):
    """Per-channel gain maps (600x800) from the stock spline coefficients."""
    g = lsc['geometry']
    bw, bh = g['block_width'], g['block_height']
    lw, lh = g['last_block_width'], g['last_block_height']
    nx, ny = g['block_xnum'] + 1, g['block_ynum'] + 1
    w, h = bw * (nx - 1) + lw, bh * (ny - 1) + lh
    coef = np.asarray(lsc['tables'][table][ct]['coef'], np.float32)

    x = np.arange(w)
    bx = np.minimum(x // bw, nx - 1)
    u = (x - bx * bw) / np.where(bx == nx - 1, lw, bw)
    y = np.arange(h)
    by = np.minimum(y // bh, ny - 1)
    v = (y - by * bh) / np.where(by == ny - 1, lh, bh)
    U, V = np.meshgrid(u.astype(np.float32), v.astype(np.float32))
    maps = []
    for ch in range(4):
        c = coef[by][:, bx, ch, :]          # h x w x 12
        c = np.moveaxis(c, -1, 0)
        gain = (c[11] / 128 + (c[9] * U + c[10] * V) / 256 +
                (c[0] * U ** 3 * V + c[1] * U * V ** 3 + c[2] * U ** 3 +
                 c[3] * U * U * V + c[4] * U * V * V + c[5] * V ** 3 +
                 c[6] * U * U + c[7] * U * V + c[8] * V * V) / 512)
        maps.append(gain.astype(np.float32))
    return maps


def light_gains(cam3a):
    """White balance gains (R/G, B/G) of the stock AWB light sources and
    preset white balance modes."""
    out = {}
    for k, v in cam3a['awb']['light_gain'].items():
        if k != 'strobe':
            out[k] = (v['r'] / v['g'], v['b'] / v['g'])
    for k, v in cam3a['awb']['pwb_gain'].items():
        if k != 'gray_world':
            out.setdefault(k, (v['r'] / v['g'], v['b'] / v['g']))
    return out


def nearest_light(gains, lights):
    rg, bg = gains
    best = min((k for k in lights if k in LSC_CT_INDEX),
               key=lambda k: (math.log(lights[k][0] / rg) ** 2 +
                              math.log(lights[k][1] / bg) ** 2))
    return best


def grey_world(p):
    """R/G and B/G gains from the mean of unclipped, non-dark pixels."""
    g = 0.5 * (p[1] + p[2])
    ok = ((p[0] < 0.95) & (p[3] < 0.95) & (g < 0.95) & (g > 0.02))
    if ok.sum() < 1000:
        ok = np.ones_like(ok)
    gm = g[ok].mean()
    return (gm / max(p[3][ok].mean(), 1e-6), gm / max(p[0][ok].mean(), 1e-6))


def stock_ccm(poly, rg, bg):
    """CCM of the stock dynamic colour correction (lib3a Interpolation()).

    The 3x3 matrix is a second-order polynomial of the normalised white
    balance gains, desaturated until the diagonal is at least 258/256, with
    the off-diagonal terms limited and every row summing to one.
    """
    x = (rg * 512 - poly['r_avg'] / 100) / (poly['r_std'] / 100)
    y = (bg * 512 - poly['b_avg'] / 100) / (poly['b_std'] / 100)
    k = [[v / 10000 for v in poly[n]]
         for n in ('p00', 'p10', 'p01', 'p20', 'p11', 'p02')]

    def m(i, x, y):
        return (k[0][i] + k[1][i] * x + k[2][i] * y + k[3][i] * x * x +
                k[4][i] * x * y + k[5][i] * y * y)

    M = [int(m(i, x, y)) for i in range(9)]
    scale = 1.0
    for d in (0, 4, 8):
        if M[d] >= 0x102:
            continue
        a = k[3][d] * x * x + k[4][d] * x * y + k[5][d] * y * y
        b = k[1][d] * x + k[2][d] * y
        c0 = k[0][d] - 258
        if a == 0:
            t = -c0 / b if b else 1.0
        else:
            disc = b * b - 4 * a * c0
            if disc < 0:
                t = 1.0
            else:
                t1 = (-b + math.sqrt(disc)) / (2 * a)
                t2 = (-b - math.sqrt(disc)) / (2 * a)
                t = t1 if abs(int(t1 - 1)) < abs(int(t2 - 1)) else t2
        scale = min(scale, t)
    if scale < 1.0:
        M = [int(m(i, x * scale, y * scale)) for i in range(9)]
    off = (1, 2, 3, 5, 6, 7)
    f = 1.0
    for a, b in ((1, 2), (3, 5), (6, 7)):
        s = abs(M[a] + M[b])
        if s >= 0x2FE:
            f = min(f, 765 / s)
    for i in off:
        if abs(M[i]) >= 0x3FE:
            f = min(f, 1021 / abs(M[i]))
    for i in off:
        M[i] = int(M[i] * f)
    M[0] = 256 - M[1] - M[2]
    M[4] = 256 - M[3] - M[5]
    M[8] = 256 - M[6] - M[7]
    return np.array(M, np.float32).reshape(3, 3) / 256


def demosaic(img):
    """Bilinear demosaic of a BGGR float mosaic, returns HxWx3 RGB."""
    h, w = img.shape
    mr = np.zeros((h, w), np.float32)
    mb = np.zeros((h, w), np.float32)
    mr[1::2, 1::2] = 1
    mb[0::2, 0::2] = 1
    mg = 1 - mr - mb

    def conv(a, k):
        p = np.pad(a, 1, mode='reflect')
        out = np.zeros_like(a)
        for dy in range(3):
            for dx in range(3):
                if k[dy][dx]:
                    out += k[dy][dx] * p[dy:dy + h, dx:dx + w]
        return out

    krb = ((1, 2, 1), (2, 4, 2), (1, 2, 1))
    kg = ((0, 1, 0), (1, 4, 1), (0, 1, 0))
    r = conv(img * mr, krb) / conv(mr, krb)
    g = conv(img * mg, kg) / conv(mg, kg)
    b = conv(img * mb, krb) / conv(mb, krb)
    return np.stack([r, g, b], -1)


def gamma_lut(isp, mode):
    x = np.arange(1024, dtype=np.float32)
    if mode == 'srgb':
        v = x / 1023
        y = np.where(v <= 0.0031308, 12.92 * v,
                     1.055 * np.power(v, 1 / 2.4) - 0.055)
        return y
    if mode == 'none':
        return x / 1023
    if mode.startswith('stock'):
        idx = int(mode[5:] or 0)
        t = isp['ggm'][idx]
        xin = t['input'] + [1024]
        yout = t['g'] + [1023]
        return np.interp(x, xin, yout).astype(np.float32) / 1023
    return np.power(x / 1023, 1 / float(mode))


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('input')
    ap.add_argument('output')
    ap.add_argument('--data', default=os.path.join(here, '..', 'data',
                                                   'sp2508'))
    ap.add_argument('--frame', type=int, default=0,
                    help='frame index in a multi-frame file')
    ap.add_argument('--black', type=float, default=16,
                    help='black level in 10-bit codes (default 16)')
    ap.add_argument('--no-crop', action='store_true',
                    help='keep the 1616x1216 frame (implies --no-lsc)')
    ap.add_argument('--no-dpc', action='store_true')
    ap.add_argument('--no-lsc', action='store_true')
    ap.add_argument('--lsc-table', default='preview',
                    choices=('preview', 'capture'))
    ap.add_argument('--lsc-ct', type=int, choices=(0, 1, 2, 3),
                    help='shading table (0 A, 1 TL84, 2 D65); '
                         'default follows the white balance')
    ap.add_argument('--lsc-strength', type=float, default=1.0)
    ap.add_argument('--no-awb', action='store_true')
    ap.add_argument('--awb', default='grey',
                    help='grey, or a stock light source / preset: horizon, '
                         'a, tl84, cwf, dnp, d65, daylight, cloudy_daylight, '
                         'shade, twilight, fluorescent, warm_fluorescent, '
                         'incandescent')
    ap.add_argument('--gains', help='manual R/G,B/G gains, e.g. 1.33,0.95')
    ap.add_argument('--no-ccm', action='store_true')
    ap.add_argument('--gamma', default='stock0',
                    help='stockN (stock GGM table N), srgb, none, or an '
                         'exponent such as 2.2')
    ap.add_argument('--exposure', type=float, default=1.0,
                    help='digital gain applied after the black level')
    ap.add_argument('-v', '--verbose', action='store_true')
    a = ap.parse_args()

    isp, cam3a, lsc = load_data(a.data)
    lights = light_gains(cam3a)

    raw = unpack_raw10(a.input, a.frame).astype(np.float32)
    if not a.no_crop:
        raw = raw[CROP_Y:CROP_Y + CROP_H, CROP_X:CROP_X + CROP_W]
    else:
        a.no_lsc = True

    img = (raw - a.black) / (WHITE - a.black)
    img = np.clip(img, 0, None)
    if not a.no_dpc:
        img = defect_clamp(img)
    img *= a.exposure
    p = [c.copy() for c in planes(img)]

    # Fixed gains, or a grey-world estimate on the shading-corrected image.
    # Without --lsc-ct the shading table follows the light source nearest to
    # the gains, starting from the TL84 table.
    if a.gains:
        fixed = tuple(float(v) for v in a.gains.split(','))
    elif a.no_awb:
        fixed = (1.0, 1.0)
    elif a.awb != 'grey':
        if a.awb not in lights:
            sys.exit('unknown light source %s' % a.awb)
        fixed = lights[a.awb]
    else:
        fixed = None

    if a.lsc_ct is not None:
        ct = a.lsc_ct
    elif fixed is not None:
        ct = LSC_CT_INDEX[nearest_light(fixed, lights)]
    else:
        ct = 1
    for _ in range(3):
        q = p
        if not a.no_lsc:
            maps = lsc_gain_maps(lsc, a.lsc_table, ct)
            q = [c * (1 + (m - 1) * a.lsc_strength) for c, m in zip(p, maps)]
        gains = fixed if fixed is not None else grey_world(q)
        if a.no_lsc or a.lsc_ct is not None:
            break
        new_ct = LSC_CT_INDEX[nearest_light(gains, lights)]
        if new_ct == ct:
            break
        ct = new_ct
    p = q

    rg, bg = gains
    p = [p[0] * bg, p[1], p[2], p[3] * rg]
    p = [np.clip(c, 0, 1) for c in p]
    rgb = demosaic(merge(p))

    if not a.no_ccm:
        ccm = stock_ccm(isp['ccm_poly22'], rg, bg)
        rgb = rgb @ ccm.T
    else:
        ccm = np.eye(3)
    rgb = np.clip(rgb, 0, 1)

    lut = gamma_lut(isp, a.gamma)
    out = lut[np.rint(rgb * 1023).astype(np.int32)]
    Image.fromarray(np.rint(out * 255).astype(np.uint8), 'RGB').save(
        a.output)

    if a.verbose:
        print('gains R/G %.3f B/G %.3f, nearest light %s, LSC table %s/%s'
              % (rg, bg, nearest_light((rg, bg), lights),
                 'off' if a.no_lsc else a.lsc_table,
                 '-' if a.no_lsc else ct))
        print('ccm', np.round(ccm, 3).tolist())


if __name__ == '__main__':
    main()
