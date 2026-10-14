# RAUC slot selection for the Fairphone 2, sourced by U-Boot from the first
# partition of the image in userdata (blkmap device ${ud_dev}):
#   2: rootfs A, 3: rootfs B, 4: RAUC state (U-Boot environment blob)
#
# This is the bootchooser of the RAUC U-Boot integration: BOOT_ORDER lists
# the slots by priority, BOOT_x_LEFT counts the attempts left for each one.
# Linux reads and writes the same variables with fw_printenv/fw_setenv.

setenv rauc_env_size 0x4000
setenv rauc_env_blocks 0x20

setenv BOOT_ORDER
setenv BOOT_A_LEFT
setenv BOOT_B_LEFT
if read blkmap ${ud_dev}:4 ${ramdisk_addr_r} 0 ${rauc_env_blocks}; then
	env import -c ${ramdisk_addr_r} ${rauc_env_size} BOOT_ORDER BOOT_A_LEFT BOOT_B_LEFT
fi
test -n "${BOOT_ORDER}" || setenv BOOT_ORDER "A B"
test -n "${BOOT_A_LEFT}" || setenv BOOT_A_LEFT 3
test -n "${BOOT_B_LEFT}" || setenv BOOT_B_LEFT 3

setenv rauc_slot
for BOOT_SLOT in "${BOOT_ORDER}"; do
	if test -n "${rauc_slot}"; then
		# a slot has been chosen already
	elif test "x${BOOT_SLOT}" = "xA"; then
		if test ${BOOT_A_LEFT} -gt 0; then
			setexpr BOOT_A_LEFT ${BOOT_A_LEFT} - 1
			setenv rauc_slot A
			setenv rauc_part 2
		fi
	elif test "x${BOOT_SLOT}" = "xB"; then
		if test ${BOOT_B_LEFT} -gt 0; then
			setexpr BOOT_B_LEFT ${BOOT_B_LEFT} - 1
			setenv rauc_slot B
			setenv rauc_part 3
		fi
	fi
done

if test -z "${rauc_slot}"; then
	echo "RAUC: no slot left to try, resetting the attempts"
	setenv BOOT_A_LEFT 3
	setenv BOOT_B_LEFT 3
fi

env export -c -s ${rauc_env_size} ${ramdisk_addr_r} BOOT_ORDER BOOT_A_LEFT BOOT_B_LEFT
write blkmap ${ud_dev}:4 ${ramdisk_addr_r} 0 ${rauc_env_blocks}

if test -z "${rauc_slot}"; then
	reset
fi

echo "RAUC: booting slot ${rauc_slot} (attempts left: A ${BOOT_A_LEFT}, B ${BOOT_B_LEFT})"
load blkmap ${ud_dev}:${rauc_part} ${kernel_addr_r} /boot/zImage
load blkmap ${ud_dev}:${rauc_part} ${fdt_addr_r} /boot/qcom-msm8974pro-fairphone-fp2.dtb
load blkmap ${ud_dev}:${rauc_part} ${ramdisk_addr_r} /boot/initramfs.cpio.gz
setenv bootargs "rauc.slot=${rauc_slot} msm.vram=192m msm.allow_vram_carveout=1 fbcon=rotate:1 fbcon=font:TER16x32 quiet panic=10"
bootz ${kernel_addr_r} ${ramdisk_addr_r}:${filesize} ${fdt_addr_r}
