#!/bin/sh
#
# Post-build script of fairphone2_uboot: installs the U-Boot boot script next
# to the kernel, in /boot of the root filesystem.

set -e

"${HOST_DIR}/bin/mkimage" -A arm -O linux -T script -C none -n "Fairphone 2 boot" \
	-d "$(dirname "$0")/boot.cmd" "${TARGET_DIR}/boot/boot.scr"
