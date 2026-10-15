#!/bin/sh
# Pack the exact Express mainline kernel, DTB and first-light initramfs for lk2nd.
set -eu

BOARD_DIR="${BR2_EXTERNAL_ANDROID_PATH}/board/samsung-expressatt"
DTB="${BINARIES_DIR}/qcom-msm8960-samsung-expressatt.dtb"
KERNEL_DTB="${BINARIES_DIR}/zImage-dtb"

if ! "${HOST_DIR}/bin/fdtget" -p "$DTB" /reserved-memory >/dev/null 2>&1; then
	"${HOST_DIR}/bin/fdtput" -c "$DTB" /reserved-memory
	"${HOST_DIR}/bin/fdtput" -t x "$DTB" /reserved-memory '#address-cells' \
		"$("${HOST_DIR}/bin/fdtget" -t x "$DTB" / '#address-cells')"
	"${HOST_DIR}/bin/fdtput" -t x "$DTB" /reserved-memory '#size-cells' \
		"$("${HOST_DIR}/bin/fdtget" -t x "$DTB" / '#size-cells')"
	"${HOST_DIR}/bin/fdtput" "$DTB" /reserved-memory ranges
fi

cat "${BINARIES_DIR}/zImage" "$DTB" > "$KERNEL_DTB"
"$BOARD_DIR/scripts/mkbootimg_v0.py" \
	"$KERNEL_DTB" \
	"${BINARIES_DIR}/rootfs.cpio.gz" \
	"${BINARIES_DIR}/boot.img"

boot_partition_bytes=${BOOT_IMAGE_MAX_BYTES:-10485760}
boot_image_bytes=$(wc -c < "${BINARIES_DIR}/boot.img")
if [ "$boot_image_bytes" -gt "$boot_partition_bytes" ]; then
	echo "boot.img is ${boot_image_bytes} bytes; limit is ${boot_partition_bytes}" >&2
	exit 1
fi

echo "boot image: ${BINARIES_DIR}/boot.img (${boot_image_bytes}/${boot_partition_bytes} bytes)"
