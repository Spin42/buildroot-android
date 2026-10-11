#!/usr/bin/env bash
# Build a rescue boot image: the same kernel with the whole root filesystem as initramfs, so the tablet runs
# entirely from RAM (usb networking and sshd as usual) and leaves userdata unmounted.
# Usage: mkrescue_image.sh [images dir] [output]   (defaults: buildroot output/images, <images>/boot-rescue.img)
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
IMAGES=${1:-$HERE/../../../../output/images}
OUT=${2:-$IMAGES/boot-rescue.img}

cat "$IMAGES/zImage" "$IMAGES/mt8127-lenovo-tb3-710f.dtb" > "$IMAGES/zImage-dtb"
# the cpio has no /init (the kernel mounts userdata in the normal boot): append one that keeps running from RAM;
# a second archive concatenated to the first is unpacked after it. xz keeps the image under the 16 MiB partition
EXTRA=$(mktemp -d)
install -m 0755 "$HERE/rescue_init" "$EXTRA/init"
(cd "$EXTRA" && echo init | cpio -o -H newc 2>/dev/null) > "$IMAGES/rescue-extra.cpio"
rm -rf "$EXTRA"
cat "$IMAGES/rootfs.cpio" "$IMAGES/rescue-extra.cpio" | xz --check=crc32 -9e -T0 -c > "$IMAGES/rootfs.cpio.xz"
"$HERE/mkbootimg.py" "$IMAGES/zImage-dtb" "$IMAGES/rootfs.cpio.xz" "$OUT"
echo "rescue image: $OUT ($(stat -c %s "$OUT") bytes)"
