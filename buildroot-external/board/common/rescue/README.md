# Generic test images (first light, rescue)

`configs/{rescue,firstlight}_{aarch64,armv7}_defconfig` build a device-independent initramfs per
architecture: busybox, the porting kit's `/init`, the USB gadget script and `reboot-mode`, plus
dropbear and the disk tools for rescue. No kernel: each device brings its own.

A device adds its bits in a second, small cpio concatenated after this one (the kernel accepts
concatenated cpio archives; later files win): its `/etc/kit-image` (port id, serial, return mode),
firmware or modules it needs early. The porting kit builds that cpio and packs the boot image.

The overlays here come from the porting kit's `templates/rescue`; keep both in sync.
