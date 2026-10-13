#!/bin/sh
#
# Post-image script of the Fairphone 2 configurations booted by U-Boot.
#
# Arguments: the images directory, then BR2_ROOTFS_POST_SCRIPT_ARGS, which are
# passed to genimage.sh (-c <genimage configuration of the userdata image>).
#
# Outputs, in $BINARIES_DIR:
#   u-boot-fp2.img - U-Boot as an Android boot image, for the boot partition
#   sdcard.img     - the userdata image

set -e

BOARD_DIR="$(dirname "$0")/.."
shift

# LK loads the image at 0x8000 and copies the device tree appended to
# u-boot.bin to 0x01e00000. The ramdisk is a placeholder LK insists on.
"${BOARD_DIR}/scripts/mkbootimg.py" \
	--kernel "${BINARIES_DIR}/u-boot.bin" \
	--output "${BINARIES_DIR}/u-boot-fp2.img"
echo "u-boot-fp2.img created at ${BINARIES_DIR}/u-boot-fp2.img"

support/scripts/genimage.sh "$@"
