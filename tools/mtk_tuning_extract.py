#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Extract the SP2508 tuning data from the stock MT8127 libcameracustom.so.

The library is only parsed, never loaded. The addresses are those used by
NSFeature::RAWSensorInfo<0x2508>::impGetDefaultData() of the Android 5.0.1
image of the Lenovo TB3-710F (see docs/tuning.md); every block is checked
against its version and sensor id before it is decoded.

usage: mtk_tuning_extract.py libcameracustom.so outdir
"""

import json
import os
import struct
import sys

SENSOR_ID = 0x2508

# impGetDefaultData() sources: (virtual address, size). Read-only data is
# mapped at file offset == virtual address; .data at file offset + 0x1000.
ISP_PARAM = (0x1DAC4, 0x5800)      # NVRAM_CAMERA_ISP_PARAM_STRUCT
CAM_3A = (0x133B44, 0x1004)        # NVRAM_CAMERA_3A_STRUCT
SHADING = (0x33328, 0x10081C)      # NVRAM_CAMERA_SHADING_STRUCT
PLINE = (0x26AB98, 0xA4)           # AE_PLINETABLE_T (.data)
TSF = (0x232C4, 0x10064)           # CAMERA_TSF_TBL_STRUCT
# strAETable records of this sensor lie in this .data range (the GC2355
# records start above it).
PLINE_RANGE = (0x264000, 0x277000)

PLINE_IDS = [
    'PREVIEW_AUTO', 'VIDEO', 'VIDEO_DYNAMIC', 'VIDEO_NIGHT',
    'VIDEO_NIGHT_DYNAMIC', 'CAPTURE_AUTO', 'CAPTURE_AUTO_ZSD',
    'CAPTURE_ISO50', 'CAPTURE_ISO50_ZSD', 'CAPTURE_ISO100',
    'CAPTURE_ISO100_ZSD', 'CAPTURE_ISO200', 'CAPTURE_ISO200_ZSD',
    'CAPTURE_ISO400', 'CAPTURE_ISO400_ZSD', 'CAPTURE_ISO800',
    'CAPTURE_ISO800_ZSD', 'CAPTURE_ISO1600', 'CAPTURE_ISO1600_ZSD',
    'CAPTURE_ISO3200', 'CAPTURE_ISO3200_ZSD',
] + ['MODE_INDEX%d' % i for i in range(1, 18)]

AE_MODES = [
    'OFF', 'AUTO', 'NIGHT', 'ACTION', 'BEACH', 'CANDLELIGHT', 'FIREWORKS',
    'LANDSCAPE', 'PORTRAIT', 'NIGHT_PORTRAIT', 'PARTY', 'SNOW', 'SPORTS',
    'STEADYPHOTO', 'SUNSET', 'THEATRE', 'ISO_ANTI_SHAKE', 'BACKLIGHT',
]

LIGHTS = ['strobe', 'horizon', 'a', 'tl84', 'cwf', 'dnp', 'd65', 'df']

# GGM input breakpoints: 64 entries of 2, 32 of 4, 32 of 8, 16 of 32 codes.
GGM_IN = ([2 * i for i in range(64)] + [128 + 4 * i for i in range(32)] +
          [256 + 8 * i for i in range(32)] + [512 + 32 * i for i in range(16)])


class Elf:
    def __init__(self, path):
        self.d = open(path, 'rb').read()
        if self.d[:4] != b'\x7fELF' or self.d[4] != 1:
            raise SystemExit('not a 32-bit ELF file')
        self.loads = []
        phoff, = struct.unpack_from('<I', self.d, 0x1C)
        phentsize, phnum = struct.unpack_from('<HH', self.d, 0x2A)
        for i in range(phnum):
            p_type, off, vaddr, _, filesz, _, _, _ = struct.unpack_from(
                '<8I', self.d, phoff + i * phentsize)
            if p_type == 1:
                self.loads.append((vaddr, off, filesz))

    def off(self, vaddr):
        for va, off, size in self.loads:
            if va <= vaddr < va + size:
                return vaddr - va + off
        raise ValueError('address 0x%x not in a loaded segment' % vaddr)

    def read(self, vaddr, size):
        o = self.off(vaddr)
        return self.d[o:o + size]

    def relative_relocs(self):
        """Return the set of addresses patched by R_ARM_RELATIVE."""
        shoff, = struct.unpack_from('<I', self.d, 0x20)
        shentsize, shnum = struct.unpack_from('<HH', self.d, 0x2E)
        out = set()
        for i in range(shnum):
            sh = struct.unpack_from('<10I', self.d, shoff + i * shentsize)
            if sh[1] != 9:              # SHT_REL
                continue
            for o in range(sh[4], sh[4] + sh[5], 8):
                r_offset, r_info = struct.unpack_from('<II', self.d, o)
                if r_info & 0xFF == 23:  # R_ARM_RELATIVE
                    out.add(r_offset)
        return out


def s11(v):
    v &= 0x7FF
    return v - 0x800 if v & 0x400 else v


def ints(b, fmt='i'):
    return list(struct.unpack('<%d%s' % (len(b) // 4, fmt), b))


def decode_isp(b):
    u = ints(b, 'I')
    i = ints(b, 'i')
    version, sid = u[0], u[1]
    if version != 1 or sid != SENSOR_ID:
        raise SystemExit('ISP block: unexpected version/sensor id')
    out = {'version': version, 'sensor_id': '0x%04x' % sid}
    out['pca_slider'] = i[66:69]
    out['pca_config'] = ['0x%08x' % v for v in u[69:71]]

    o = 0x98C
    names = ['OBC', 'BPC', 'NR1', 'LSC', 'SL2', 'CFA', 'CCM', 'GGM', 'ANR',
             'CCR', 'EE']
    out['index'] = dict(zip(names, b[o:o + 11]))
    o += 12

    def regs(count, words):
        nonlocal o
        r = [u[o // 4 + k * words:o // 4 + (k + 1) * words]
             for k in range(count)]
        o += count * words * 4
        return r

    obc = regs(12, 8)
    out['obc'] = [{
        'offset_raw': ['0x%04x' % v for v in e[:4]],
        'offset_13bit': [(v & 0x1FFF) - 0x2000 if v & 0x1000 else v & 0x1FFF
                         for v in e[:4]],
        'gain': [round((v & 0x1FFF) / 512, 4) for v in e[4:]],
    } for e in obc]
    out['bpc'] = [['0x%08x' % v for v in e] for e in regs(9, 12)]
    out['nr1'] = [['0x%08x' % v for v in e] for e in regs(9, 2)]
    lsc = regs(6, 10)
    out['lsc_regs'] = []
    for e in lsc:
        ctl1, ctl2, ctl3, lblock, ratio, gain_th = e[4:10]
        out['lsc_regs'].append({
            'raw': ['0x%08x' % v for v in e],
            'block_xoffset': (ctl1 >> 16) & 0x3F,
            'block_yoffset': ctl1 & 0x3F,
            'block_width': ctl2 & 0xFFF,
            'block_xnum': (ctl2 >> 12) & 0x1F,
            'block_height': ctl3 & 0xFFF,
            'block_ynum': (ctl3 >> 12) & 0x1F,
            'last_block_width': (lblock >> 16) & 0xFFF,
            'last_block_height': lblock & 0xFFF,
            'ratio': [(ratio >> s) & 0x3F for s in (24, 16, 8, 0)],
        })
    out['sl2'] = [['0x%08x' % v for v in e] for e in regs(24, 4)]
    out['cfa'] = [['0x%08x' % v for v in e] for e in regs(43, 21)]
    out['ccm_regs'] = []
    for e in regs(4, 6):
        m = [s11(e[0]), s11(e[0] >> 16), s11(e[1]),
             s11(e[2]), s11(e[2] >> 16), s11(e[3]),
             s11(e[4]), s11(e[4] >> 16), s11(e[5])]
        out['ccm_regs'].append({'q8': m, 'matrix': [round(v / 256, 4)
                                                    for v in m]})
    out['ggm'] = []
    for e in regs(5, 288):
        rb, g = e[:144], e[144:]
        out['ggm'].append({
            'input': GGM_IN,
            'r': [v & 0x3FF for v in rb],
            'b': [(v >> 16) & 0x3FF for v in rb],
            'g': [v & 0x3FF for v in g],
        })
    out['anr'] = [['0x%08x' % v for v in e] for e in regs(49, 18)]
    out['ccr'] = [['0x%08x' % v for v in e] for e in regs(7, 4)]
    out['ee'] = [['0x%08x' % v for v in e] for e in regs(49, 16)]
    out['mfb_mixer'] = i[o // 4:o // 4 + 14]
    o += 56
    p = i[o // 4:o // 4 + 58]
    out['ccm_poly22'] = {
        'r_avg': p[0], 'r_std': p[1], 'b_avg': p[2], 'b_std': p[3],
        'p00': p[4:13], 'p10': p[13:22], 'p01': p[22:31],
        'p20': p[31:40], 'p11': p[40:49], 'p02': p[49:58],
    }
    return out


def decode_3a(b):
    w = ints(b)
    if w[0] != 1 or w[1] != SENSOR_ID:
        raise SystemExit('3A block: unexpected version/sensor id')
    dev = dict(zip([
        'min_gain', 'max_gain', 'mini_iso_gain', 'gain_step_unit',
        'preview_exp_unit_ns', 'preview_max_fps', 'video_exp_unit_ns',
        'video_max_fps', 'video_to_preview_ratio', 'capture_exp_unit_ns',
        'capture_max_fps', 'capture_to_preview_ratio', 'lens_fno_x10',
        'focal_length_x100'], w[2:16]))
    hist = {
        'hist_high_thres': w[16], 'hist_low_thres': w[17],
        'most_bright_ratio': w[18], 'most_dark_ratio': w[19],
        'central_high_bound': w[20], 'central_low_bound': w[21],
        'over_exp_thres': w[22:27], 'hist_stretch_thres': w[27:32],
        'black_light_thres': w[32:37],
    }
    cct = dict(zip([
        'enable_black_light', 'enable_hist_stretch',
        'enable_anti_over_exposure', 'enable_time_lpf',
        'enable_capture_thres', 'enable_video_thres', 'enable_strobe_thres',
        'ae_target', 'strobe_ae_target', 'init_index', 'back_light_weight',
        'hist_stretch_weight', 'anti_over_exp_weight',
        'black_light_strength_index', 'hist_stretch_strength_index',
        'anti_over_exp_strength_index', 'time_lpf_strength_index'],
        w[37:54]))
    cct['lpf_converge_level'] = w[54:59]
    cct.update(zip([
        'indoor_ev', 'bv_offset', 'preview_flare_offset',
        'capture_flare_offset', 'capture_flare_thres', 'video_flare_offset',
        'video_flare_thres', 'strobe_flare_offset', 'strobe_flare_thres',
        'preview_max_flare_thres', 'preview_min_flare_thres',
        'video_max_flare_thres', 'video_min_flare_thres', 'flatness_thres',
        'flatness_strength'], w[59:74]))

    o = 74

    def take(n):
        nonlocal o
        v = w[o:o + n]
        o += n
        return v

    def gain3(v):
        return {'r': v[0], 'g': v[1], 'b': v[2]}

    awb = {}
    awb['calibration'] = {k: gain3(take(3)) for k in
                          ('unit', 'golden', 'tuning_unit', 'd65')}
    awb['original_xy'] = {k: take(2) for k in LIGHTS}
    awb['rotated_xy'] = {k: take(2) for k in LIGHTS}
    awb['light_gain'] = {k: gain3(take(3)) for k in LIGHTS}
    awb['rotation'] = dict(zip(['angle', 'cos', 'sin'], take(3)))
    awb['daylight_locus'] = dict(zip(['slope_num', 'slope_den'], take(2)))
    awb['light_area'] = {k: dict(zip(['right', 'left', 'upper', 'lower'],
                                     take(4)))
                         for k in ('strobe', 'tungsten', 'warm_fluorescent',
                                   'fluorescent', 'cwf', 'daylight', 'shade',
                                   'daylight_fluorescent')}
    awb['pwb_area'] = {k: dict(zip(['right', 'left', 'upper', 'lower'],
                                   take(4)))
                       for k in ('reference', 'daylight', 'cloudy_daylight',
                                 'shade', 'twilight', 'fluorescent',
                                 'warm_fluorescent', 'incandescent',
                                 'gray_world')}
    awb['pwb_gain'] = {k: gain3(take(3))
                       for k in ('daylight', 'cloudy_daylight', 'shade',
                                 'twilight', 'fluorescent', 'warm_fluorescent',
                                 'incandescent', 'gray_world')}
    pref = {k: dict(zip(['slider', 'offset_thr'], take(2)))
            for k in ('tungsten', 'warm_fluorescent', 'shade')}
    pref['daylight_wb_gain'] = gain3(take(3))
    pref['preference_gain'] = {k: gain3(take(3)) for k in LIGHTS}
    awb['preference'] = pref
    awb['cct_estimation'] = {'cct': take(5), 'rotated_x': take(5)}
    return {'version': w[0], 'sensor_id': '0x%04x' % w[1],
            'ae': {'devices': dev, 'histogram': hist, 'cct': cct},
            'awb': awb}


def decode_pline(elf):
    relocs = elf.relative_relocs()

    def u32(a, n=1):
        return list(struct.unpack('<%dI' % n, elf.read(a, 4 * n)))

    mapping_ptr = u32(PLINE[0])[0]
    if PLINE[0] not in relocs:
        raise SystemExit('pline: mapping pointer is not relocated')
    mapping = []
    a = mapping_ptr
    for _ in range(len(AE_MODES) + 2):
        mode, pid, zid = struct.unpack('<3i', elf.read(a, 12))
        if mode < 0:
            break
        mapping.append({'ae_mode': AE_MODES[mode] if mode < len(AE_MODES)
                        else mode,
                        'pline': PLINE_IDS[pid], 'zsd_pline': PLINE_IDS[zid]})
        a += 12

    tables = []
    for r in sorted(relocs):
        if not PLINE_RANGE[0] <= r < PLINE_RANGE[1]:
            continue
        if r + 4 not in relocs or r - 4 in relocs:
            continue
        s = r - 0x18
        eid, total, strobe_bv, max_bv, min_bv, iso = struct.unpack(
            '<6i', elf.read(s, 24))
        if not 0 <= eid < len(PLINE_IDS) or not 0 < total < 512:
            continue
        p60, p50 = u32(r, 2)

        def rows(p):
            out = []
            for k in range(total):
                e, afe, isp = u32(p + 16 * k, 3)
                out.append([e, afe, isp])
            return out
        tables.append({'id': PLINE_IDS[eid], 'iso': iso, 'max_bv': max_bv,
                       'min_bv': min_bv, 'strobe_trigger_bv': strobe_bv,
                       'rows_60hz': rows(p60), 'rows_50hz': rows(p50)})
    tables.sort(key=lambda t: PLINE_IDS.index(t['id']))
    return {'row_format': ['exposure_us', 'sensor_gain_x1024',
                           'isp_gain_x1024'],
            'mode_mapping': mapping, 'tables': tables}


def decode_lsc_words(words):
    """Return [by][bx][ch][12] signed coefficients from 16x16x4x4 words."""
    out = []
    for by in range(16):
        row = []
        for bx in range(16):
            blk = []
            for ch in range(4):
                c = []
                base = ((by * 16 + bx) * 4 + ch) * 4
                for k in range(4):
                    v = words[base + k]
                    for s in (0, 10, 20):
                        f = (v >> s) & 0x3FF
                        c.append(-(f & 0x1FF) if f & 0x200 else f)
                blk.append(c)
            row.append(blk)
        out.append(row)
    return out


def lsc_eval(c, u, v):
    return (c[11] / 128 + (c[9] * u + c[10] * v) / 256 +
            (c[0] * u ** 3 * v + c[1] * u * v ** 3 + c[2] * u ** 3 +
             c[3] * u * u * v + c[4] * u * v * v + c[5] * v ** 3 +
             c[6] * u * u + c[7] * u * v + c[8] * v * v) / 512)


def lsc_grid(coef):
    """Gains at the 17x17 block corners, [ch][y][x]."""
    g = [[[0.0] * 17 for _ in range(17)] for _ in range(4)]
    for ch in range(4):
        for y in range(17):
            for x in range(17):
                by, u = (y, 0.0) if y < 16 else (15, 1.0)
                bx, t = (x, 0.0) if x < 16 else (15, 1.0)
                g[ch][y][x] = round(lsc_eval(coef[by][bx][ch], t, u), 4)
    return g


def decode_shading(b, isp):
    version, sid = struct.unpack_from('<II', b, 0)
    if version != 1:
        raise SystemExit('shading block: unexpected version')
    words = ints(b[0x1C:0x1C + 6 * 4 * 4096 * 4], 'I')
    names = ['preview', 'capture', 'capture_tile', 'video', 'n3d_preview',
             'n3d_capture']
    out = {
        'version': version,
        'sensor_id_field': sid,
        'lsc_size': list(struct.unpack_from('<6H', b, 8)),
        'svd_size': list(struct.unpack_from('<3H', b, 20)),
        'geometry': isp['lsc_regs'][0],
        'channel_order': ['B', 'Gb', 'Gr', 'R'],
        'coefficient_format': (
            'per block and channel 4 words, 3 fields of 10 bits each '
            '(bit 9 sign, bits 8..0 magnitude), c0..c11 in field order; '
            'gain(u,v) = c11/128 + (c9*u + c10*v)/256 + (c0*u^3*v + '
            'c1*u*v^3 + c2*u^3 + c3*u^2*v + c4*u*v^2 + c5*v^3 + c6*u^2 + '
            'c7*u*v + c8*v^2)/512, u and v in [0,1] across the block'),
        'tables': {},
    }
    for t, name in enumerate(names):
        cts = []
        for ct in range(4):
            w = words[(t * 4 + ct) * 4096:(t * 4 + ct + 1) * 4096]
            if not any(w):
                cts.append(None)
                continue
            coef = decode_lsc_words(w)
            cts.append({'coef': coef, 'corner_gain': lsc_grid(coef)})
        if any(cts):
            out['tables'][name] = cts
    return out


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    elf = Elf(sys.argv[1])
    outdir = sys.argv[2]
    os.makedirs(outdir, exist_ok=True)

    isp = decode_isp(elf.read(*ISP_PARAM))
    cam3a = decode_3a(elf.read(*CAM_3A))
    pline = decode_pline(elf)
    shading = decode_shading(elf.read(*SHADING), isp)

    def dump(name, obj, indent=1):
        with open(os.path.join(outdir, name), 'w') as f:
            json.dump(obj, f, indent=indent, separators=(',', ':'))
            f.write('\n')

    dump('isp_param.json', isp)
    dump('3a_param.json', cam3a)
    dump('ae_pline.json', pline)
    dump('lsc.json', shading, indent=None)


if __name__ == '__main__':
    main()
