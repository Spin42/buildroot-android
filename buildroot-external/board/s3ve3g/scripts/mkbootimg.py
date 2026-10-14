#!/usr/bin/env python3
"""Minimal Android boot image v0 packer (the AOSP mkbootimg layout, no second stage, no QCDT).

mkbootimg.py --kernel K --ramdisk R --output OUT [--cmdline STR] [--base A] [--pagesize N]
             [--kernel_offset A] [--ramdisk_offset A] [--second_offset A] [--tags_offset A]
"""
import argparse
import hashlib
import struct


def num(s):
    return int(s, 0)


ap = argparse.ArgumentParser()
ap.add_argument("--kernel", required=True)
ap.add_argument("--ramdisk", required=True)
ap.add_argument("--output", required=True)
ap.add_argument("--cmdline", default="")
ap.add_argument("--name", default="")
ap.add_argument("--base", type=num, default=0x10000000)
ap.add_argument("--pagesize", type=num, default=2048)
ap.add_argument("--kernel_offset", type=num, default=0x00008000)
ap.add_argument("--ramdisk_offset", type=num, default=0x01000000)
ap.add_argument("--second_offset", type=num, default=0x00f00000)
ap.add_argument("--tags_offset", type=num, default=0x00000100)
a = ap.parse_args()

if len(a.cmdline) > 511:
    raise SystemExit("cmdline longer than 511 bytes")

kernel = open(a.kernel, "rb").read()
ramdisk = open(a.ramdisk, "rb").read()
second = b""

sha = hashlib.sha1()
for blob in (kernel, ramdisk, second):
    sha.update(blob)
    sha.update(struct.pack("<I", len(blob)))

hdr = b"ANDROID!"
hdr += struct.pack(
    "<10I",
    len(kernel), a.base + a.kernel_offset,
    len(ramdisk), a.base + a.ramdisk_offset,
    len(second), a.base + a.second_offset,
    a.base + a.tags_offset,
    a.pagesize,
    0,  # header_version 0
    0,  # os_version
)
hdr += a.name.encode().ljust(16, b"\0")
hdr += a.cmdline.encode().ljust(512, b"\0")
hdr += sha.digest().ljust(32, b"\0")
hdr += b"\0" * 1024  # extra_cmdline


def pad(blob):
    return blob + b"\0" * (-len(blob) % a.pagesize)


with open(a.output, "wb") as f:
    f.write(pad(hdr) + pad(kernel) + pad(ramdisk))
print(f"{a.output}: kernel {len(kernel)} ramdisk {len(ramdisk)}")
