#!/usr/bin/env python3
"""Minimal Android boot image v0 packer. mkbootimg.py kernel ramdisk out [--cmdline STR] [--mtk-hdr]
Addresses match the stock TB3-710F image (kernel 0x80008000, ramdisk 0x84000000, tags 0x80000100, page 2048).
--mtk-hdr wraps the kernel and ramdisk in the 512-byte MediaTek header (magic 0x58881688) like the stock images."""
import struct, sys, hashlib, argparse
ap = argparse.ArgumentParser(); ap.add_argument('kernel'); ap.add_argument('ramdisk'); ap.add_argument('out')
ap.add_argument('--cmdline', default=''); ap.add_argument('--mtk-hdr', action='store_true'); ap.add_argument('--name', default='')
a = ap.parse_args()
PAGE = 2048
def mtk(b, name):
    h = struct.pack('<II32s', 0x58881688, len(b), name.encode()) ; return h.ljust(512, b'\xff') + b
k = open(a.kernel, 'rb').read(); r = open(a.ramdisk, 'rb').read()
if a.mtk_hdr: k = mtk(k, 'KERNEL'); r = mtk(r, 'ROOTFS')
pad = lambda b: b + b'\0' * (-len(b) % PAGE)
sha = hashlib.sha1(); [sha.update(x + struct.pack('<I', len(x))) for x in (k, r, b'')]
hdr = b'ANDROID!' + struct.pack('<10I', len(k), 0x80008000, len(r), 0x84000000, 0, 0x80f00000, 0x80000100, PAGE, 0, 0)
hdr += a.name.encode().ljust(16, b'\0') + a.cmdline.encode().ljust(512, b'\0') + sha.digest().ljust(32, b'\0')
open(a.out, 'wb').write(pad(hdr) + pad(k) + pad(r))
print(f'{a.out}: kernel {len(k)} ramdisk {len(r)}')
