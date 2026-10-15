#!/bin/sh
set -eu

# Rescue boots from RAM. Keep its initrd within 0x84000000..0x85000000;
# this image is not sized for the physical 10 MiB BOOT partition.
ramdisk_bytes=$(wc -c < "${BINARIES_DIR}/rootfs.cpio.gz")
if [ "$ramdisk_bytes" -gt 16777216 ]; then
	echo "rescue initrd exceeds its 16 MiB RAM budget: ${ramdisk_bytes}" >&2
	exit 1
fi
BOOT_IMAGE_MAX_BYTES=33554432 \
	exec "${BR2_EXTERNAL_ANDROID_PATH}/board/samsung-expressatt/post-image.sh" "$@"
