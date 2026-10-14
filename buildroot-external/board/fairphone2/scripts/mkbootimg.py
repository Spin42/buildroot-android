#!/usr/bin/env python3
# Minimal Android boot image (header v0) writer for the Fairphone 2 LK.
import argparse
import hashlib
import struct

parser = argparse.ArgumentParser()
parser.add_argument("--kernel", required=True)
parser.add_argument("--ramdisk")
parser.add_argument("--cmdline", default="")
parser.add_argument("--base", type=lambda x: int(x, 0), default=0x00000000)
parser.add_argument("--kernel-offset", type=lambda x: int(x, 0), default=0x00008000)
parser.add_argument("--ramdisk-offset", type=lambda x: int(x, 0), default=0x02000000)
parser.add_argument("--tags-offset", type=lambda x: int(x, 0), default=0x01e00000)
parser.add_argument("--pagesize", type=int, default=2048)
parser.add_argument("--output", required=True)
args = parser.parse_args()

kernel = open(args.kernel, "rb").read()
# LK refuses images without a ramdisk
ramdisk = open(args.ramdisk, "rb").read() if args.ramdisk else b"\0"
cmdline = args.cmdline.encode()
if len(cmdline) > 511:
    raise SystemExit("cmdline too long")

sha = hashlib.sha1()
for blob in (kernel, ramdisk, b""):
    sha.update(blob)
    sha.update(struct.pack("<I", len(blob)))
image_id = sha.digest() + b"\0" * 12

header = struct.pack(
    "<8s10I16s512s32s1024s",
    b"ANDROID!",
    len(kernel), args.base + args.kernel_offset,
    len(ramdisk), args.base + args.ramdisk_offset,
    0, args.base + 0x00f00000,
    args.base + args.tags_offset, args.pagesize,
    0,  # header version
    0,  # os version
    b"",  # board name
    cmdline,
    image_id,
    b"",  # extra cmdline
)


def pad(data):
    return data + b"\0" * (-len(data) % args.pagesize)


with open(args.output, "wb") as out:
    out.write(pad(header))
    out.write(pad(kernel))
    out.write(pad(ramdisk))
