#!/bin/sh
# Post-image script: build the Android-style boot.img LK boots on the Lenovo TB3-710F.
# kernel = zImage with the DTB appended (LK has no DT support), ramdisk = an empty initramfs: the kernel
# mounts the root filesystem on userdata (mmcblk0p7) itself, see CONFIG_CMDLINE in linux.config.
set -e

BOARD_DIR="${BR2_EXTERNAL_ANDROID_PATH}/board/tb3_710f"
DTB="${BINARIES_DIR}/mt8127-lenovo-tb3-710f.dtb"
EMPTY="${BINARIES_DIR}/empty.cpio.gz"

cat "${BINARIES_DIR}/zImage" "${DTB}" > "${BINARIES_DIR}/zImage-dtb"
# LK wants a ramdisk in the image; an archive with no entries keeps it tiny
printf '' | cpio -o -H newc 2>/dev/null | gzip -9 > "${EMPTY}"

"${BOARD_DIR}/scripts/mkbootimg.py" \
	"${BINARIES_DIR}/zImage-dtb" \
	"${EMPTY}" \
	"${BINARIES_DIR}/boot.img"

echo "boot.img created at ${BINARIES_DIR}/boot.img"
