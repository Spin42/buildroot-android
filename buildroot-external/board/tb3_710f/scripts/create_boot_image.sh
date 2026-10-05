#!/bin/sh
# Post-image script: build the Android-style boot.img LK boots on the Lenovo TB3-710F.
# kernel = zImage with the DTB appended (LK has no DT support), ramdisk = the rootfs cpio, which
# is an initramfs whose /init switches to the persistent root on userdata (or stays as rescue).
set -e

BOARD_DIR="${BR2_EXTERNAL_ANDROID_PATH}/board/tb3_710f"
DTB="${BINARIES_DIR}/mt8127-lenovo-tb3-710f.dtb"

cat "${BINARIES_DIR}/zImage" "${DTB}" > "${BINARIES_DIR}/zImage-dtb"

"${BOARD_DIR}/scripts/mkbootimg.py" \
	"${BINARIES_DIR}/zImage-dtb" \
	"${BINARIES_DIR}/rootfs.cpio.gz" \
	"${BINARIES_DIR}/boot.img"

echo "boot.img created at ${BINARIES_DIR}/boot.img"
