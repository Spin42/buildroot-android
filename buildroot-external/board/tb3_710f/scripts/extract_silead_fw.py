#!/usr/bin/env python3
"""Convert the GSL3670 firmware arrays of the stock kernel source into the
file format of the mainline silead driver (little-endian u32 offset, u32 value
pairs).

usage: extract_silead_fw.py <gsl_ts_fw.h> <array name, e.g. GSLX68X_FW_1> <output .fw>

The header is in drivers/input/touchscreen/mediatek/GSL3670/ of the stock
kernel source. The firmware is Silead's: it is not part of this repository.
"""
import re
import struct
import sys

src, name, out = sys.argv[1:4]
text = open(src, encoding="latin-1").read()
m = re.search(r"%s\s*\[\]\s*=\s*(?://[^\n]*\n\s*)*\{(.*?)\n\};" % re.escape(name), text, re.S)
if not m:
    sys.exit("array %s not found" % name)
pairs = re.findall(r"\{\s*(0x[0-9a-fA-F]+)\s*,\s*(0x[0-9a-fA-F]+)\s*\}", m.group(1))
with open(out, "wb") as f:
    for off, val in pairs:
        f.write(struct.pack("<II", int(off, 16), int(val, 16)))
print("%s: %d entries -> %s" % (name, len(pairs), out))
