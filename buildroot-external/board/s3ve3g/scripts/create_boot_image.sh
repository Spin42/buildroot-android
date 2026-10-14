#!/bin/sh
# Post-image script: pack the Android v0 boot.img that lk2nd boots with "fastboot boot" (RAM) or
# from its "boot" partition (BOOT + 512 KiB). kernel = zImage with the board DTB appended: lk2nd
# takes a single appended DTB without qcom,msm-id, patches it (memory, cmdline, ramoops) and clears
# its magic so the kernel uses the patched copy. No QCDT and no SEANDROIDENFORCE footer: those are
# only for the stock Samsung aboot, which loads lk2nd, not this image.
set -e

BOARD_DIR="${BR2_EXTERNAL_ANDROID_PATH}/board/s3ve3g"
DTB="${BINARIES_DIR}/qcom-msm8226-samsung-s3ve3g.dtb"
# The kernel unpacks either: gzip for first light, xz (smaller, slower) for the rescue system.
if grep -q '^BR2_TARGET_ROOTFS_CPIO_XZ=y' "${BR2_CONFIG}"; then
	RAMDISK="${BINARIES_DIR}/rootfs.cpio.xz"
else
	RAMDISK="${BINARIES_DIR}/rootfs.cpio.gz"
fi
OUT="${BINARIES_DIR}/boot.img"
# lk2nd's "boot" partition: the 10 MiB BOOT minus the 512 KiB lk2nd occupies
MAX_SIZE=$((0x980000))

# clk_ignore_unused/pd_ignore_unused: keep the clocks and power domains of the display the
# bootloader left on, since simple-framebuffer does not claim them.
# lk2nd.pass-ramoops: lk2nd adds a ramoops region, read back with "fastboot oem ramoops".
# panic=10: reboot 10 s after a panic; with nothing bootable after lk2nd, that lands in its fastboot.
# reboot=warm: a hard reset loses IMEM, where reboot-mode leaves the magic for the Samsung aboot.
CMDLINE="console=tty0 loglevel=8 clk_ignore_unused pd_ignore_unused panic=10 reboot=warm qcom_smbb.trace=0 lk2nd.pass-ramoops"

cat "${BINARIES_DIR}/zImage" "${DTB}" > "${BINARIES_DIR}/zImage-dtb"

# Offsets: port.yaml device.bootimg (stock s3ve3g layout)
"${BOARD_DIR}/scripts/mkbootimg.py" \
	--kernel "${BINARIES_DIR}/zImage-dtb" \
	--ramdisk "${RAMDISK}" \
	--cmdline "${CMDLINE}" \
	--base 0x00000000 \
	--pagesize 2048 \
	--kernel_offset 0x00008000 \
	--ramdisk_offset 0x02000000 \
	--second_offset 0x00f00000 \
	--tags_offset 0x01e00000 \
	--output "${OUT}"

size=$(stat -c %s "${OUT}")
if [ "${size}" -gt "${MAX_SIZE}" ]; then
	echo "boot.img is ${size} bytes, larger than lk2nd's boot partition (${MAX_SIZE})" >&2
	exit 1
fi
(cd "${BINARIES_DIR}" && sha256sum boot.img > boot.img.sha256)
echo "boot.img created at ${OUT} (${size} bytes)"
