# Boot script of fairphone2_uboot, in /boot of the root filesystem, found by
# U-Boot's standard boot which sets devtype, devnum, distro_bootpart and
# prefix (the directory of this script).

part uuid ${devtype} ${devnum}:${distro_bootpart} rootuuid
setenv bootargs "root=PARTUUID=${rootuuid} rootfstype=ext3 rw rootwait msm.vram=192m msm.allow_vram_carveout=1 fbcon=rotate:1 fbcon=font:TER16x32 quiet panic=10"
load ${devtype} ${devnum}:${distro_bootpart} ${kernel_addr_r} ${prefix}zImage
load ${devtype} ${devnum}:${distro_bootpart} ${fdt_addr_r} ${prefix}qcom-msm8974pro-fairphone-fp2.dtb
bootz ${kernel_addr_r} - ${fdt_addr_r}
